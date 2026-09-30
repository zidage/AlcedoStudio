//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_lut_adjustment_model.hpp"

#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {
namespace {

constexpr double kPercent = 100.0;

}  // namespace

EditorLutAdjustmentModel::EditorLutAdjustmentModel(QObject* parent)
    : EditorAdjustmentValueModel(parent) {
  setFieldKey(QStringLiteral("lut"));
  setLabel(QStringLiteral("LUT strength"));
  setMinimum(0.0);
  setMaximum(kPercent);
  setDefaultValue(kPercent);
  setStep(1.0);
  setPrecision(0);
  setSuffix(QStringLiteral("%"));
  setValue(kPercent);
}

void EditorLutAdjustmentModel::setTarget(LutLibraryController* target) {
  if (target_ == target) return;
  if (target_) disconnect(target_, nullptr, this, nullptr);
  target_ = target;
  if (target_) {
    connect(target_, &LutLibraryController::associationChanged, this,
            &EditorLutAdjustmentModel::loadFromTarget);
  }
  emit targetChanged();
  loadFromTarget();
}

void EditorLutAdjustmentModel::loadFromTarget() {
  // A load during a pointer drag would move the value under the pointer; the settled
  // write reloads it afterwards.
  if (!dragActive()) {
    setValue(target_ ? target_->associationStrength() * kPercent : kPercent);
  }
  emit associationChanged();
}

auto EditorLutAdjustmentModel::hasAssociation() const -> bool {
  return target_ && target_->hasAssociation();
}

auto EditorLutAdjustmentModel::associationName() const -> QString {
  return target_ ? target_->associationName() : QString{};
}

auto EditorLutAdjustmentModel::missing() const -> bool {
  return target_ && target_->associationMissing();
}

auto EditorLutAdjustmentModel::statusText() const -> QString {
  if (!hasAssociation()) return Tr("No LUT");
  if (missing()) return Tr("Missing: %1").arg(associationName());
  return associationName();
}

auto EditorLutAdjustmentModel::valueWrite(double v) const -> alcedo::EditorParameterWrite {
  alcedo::EditorLutWrite write;
  write.strength = static_cast<float>(v / kPercent);
  return write;
}

}  // namespace alcedo::ui
