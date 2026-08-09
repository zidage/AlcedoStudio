//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_color_temp_model.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantMap>
#include <algorithm>
#include <cmath>

#include "ui/alcedo_main/editor_support/modules/color_temp.hpp"

namespace alcedo::ui {
namespace {

auto ModeString(int mode_index) -> QString {
  return mode_index == 1 ? QStringLiteral("custom") : QStringLiteral("as_shot");
}

auto ModeIndexFromString(const QString& mode) -> int {
  if (mode.compare(QStringLiteral("custom"), Qt::CaseInsensitive) == 0) {
    return 1;
  }
  return 0;
}

auto ClampCct(double kelvin) -> double {
  return std::clamp(kelvin, static_cast<double>(color_temp::kCctMin),
                    static_cast<double>(color_temp::kCctMax));
}

auto ClampTint(double value) -> double {
  return std::clamp(value, static_cast<double>(color_temp::kTintMin),
                    static_cast<double>(color_temp::kTintMax));
}

}  // namespace

EditorColorTempModel::EditorColorTempModel(QObject* parent) : EditorAdjustmentModelBase(parent) {
  setFieldKey(QStringLiteral("color_temp"));
  setLabel(QStringLiteral("White Balance"));
}

void EditorColorTempModel::setModeIndex(int index) {
  const int clamped = index == 1 ? 1 : 0;
  if (modeIndex_ == clamped) {
    return;
  }
  modeIndex_ = clamped;
  if (modeIndex_ == 0) {
    cct_  = asShotCct_;
    tint_ = asShotTint_;
    emit cctChanged();
    emit tintChanged();
  }
  emit modeIndexChanged();
}

auto EditorColorTempModel::modeValue() const -> QString { return ModeString(modeIndex_); }

void EditorColorTempModel::setCct(double kelvin) {
  const double next = ClampCct(kelvin);
  if (std::abs(next - cct_) < 1e-6) {
    return;
  }
  cct_ = next;
  emit cctChanged();
}

void EditorColorTempModel::setTint(double value) {
  const double next = ClampTint(value);
  if (std::abs(next - tint_) < 1e-6) {
    return;
  }
  tint_ = next;
  emit tintChanged();
}

void EditorColorTempModel::setAsShotCct(double kelvin) {
  const double next = ClampCct(kelvin);
  if (std::abs(next - asShotCct_) < 1e-6) {
    return;
  }
  asShotCct_ = next;
  if (modeIndex_ == 0) {
    cct_ = asShotCct_;
    emit cctChanged();
  }
  emit asShotCctChanged();
}

void EditorColorTempModel::setAsShotTint(double value) {
  const double next = ClampTint(value);
  if (std::abs(next - asShotTint_) < 1e-6) {
    return;
  }
  asShotTint_ = next;
  if (modeIndex_ == 0) {
    tint_ = asShotTint_;
    emit tintChanged();
  }
  emit asShotTintChanged();
}

void EditorColorTempModel::setSupported(bool supported) {
  if (supported_ == supported) {
    return;
  }
  supported_ = supported;
  emit supportedChanged();
}

auto EditorColorTempModel::cctSliderPos() const -> int {
  return color_temp::CctToSliderPos(static_cast<float>(cct_));
}

void EditorColorTempModel::setCctSliderPos(int pos) { setCct(color_temp::SliderPosToCct(pos)); }

void EditorColorTempModel::selectMode(int index) {
  const int clamped = index == 1 ? 1 : 0;
  if (clamped == modeIndex_) {
    return;
  }
  if (clamped == 1 && modeIndex_ == 0) {
    // Capture displayed As Shot values as the custom starting point.
    cct_  = asShotCct_;
    tint_ = asShotTint_;
  }
  modeIndex_ = clamped;
  if (modeIndex_ == 0) {
    cct_  = asShotCct_;
    tint_ = asShotTint_;
  }
  emit modeIndexChanged();
  emit cctChanged();
  emit tintChanged();
  submitSettled();
  emit settledCommitted();
}

void EditorColorTempModel::beginCctDrag() {
  cctDragMoved_ = false;
  setDragActive(true, DragTarget::Cct);
}

void EditorColorTempModel::updateCctDrag(double kelvin) {
  if (!dragActive_ || dragTarget_ != DragTarget::Cct) {
    return;
  }
  promoteToCustomForEditing();
  const double next = ClampCct(kelvin);
  if (std::abs(next - cct_) < 1e-6) {
    return;
  }
  cct_          = next;
  cctDragMoved_ = true;
  emit cctChanged();
  submitInteractive();
}

void EditorColorTempModel::updateCctSliderDrag(int pos) {
  updateCctDrag(color_temp::SliderPosToCct(pos));
}

void EditorColorTempModel::finishCctDrag() {
  if (!dragActive_ || dragTarget_ != DragTarget::Cct) {
    return;
  }
  setDragActive(false, DragTarget::None);
  // Empty press/release (including the first half of a double-click) must not
  // settle — reset() owns the double-click commit. Settling here + reset causes
  // back-to-back history commits while a render may hold the pipeline lock.
  if (!cctDragMoved_) {
    return;
  }
  cctDragMoved_ = false;
  submitSettled();
  emit settledCommitted();
}

void EditorColorTempModel::beginTintDrag() {
  tintDragMoved_ = false;
  setDragActive(true, DragTarget::Tint);
}

void EditorColorTempModel::updateTintDrag(double value) {
  if (!dragActive_ || dragTarget_ != DragTarget::Tint) {
    return;
  }
  promoteToCustomForEditing();
  const double next = ClampTint(value);
  if (std::abs(next - tint_) < 1e-6) {
    return;
  }
  tint_          = next;
  tintDragMoved_ = true;
  emit tintChanged();
  submitInteractive();
}

void EditorColorTempModel::finishTintDrag() {
  if (!dragActive_ || dragTarget_ != DragTarget::Tint) {
    return;
  }
  setDragActive(false, DragTarget::None);
  if (!tintDragMoved_) {
    return;
  }
  tintDragMoved_ = false;
  submitSettled();
  emit settledCommitted();
}

void EditorColorTempModel::editCct(double kelvin) {
  promoteToCustomForEditing();
  const double next = ClampCct(kelvin);
  if (std::abs(next - cct_) > 1e-6) {
    cct_ = next;
    emit cctChanged();
  }
  submitInteractive();
  submitSettled();
  emit settledCommitted();
}

void EditorColorTempModel::editTint(double value) {
  promoteToCustomForEditing();
  const double next = ClampTint(value);
  if (std::abs(next - tint_) > 1e-6) {
    tint_ = next;
    emit tintChanged();
  }
  submitInteractive();
  submitSettled();
  emit settledCommitted();
}

void EditorColorTempModel::commitImmediately() {
  if (dragActive_) {
    setDragActive(false, DragTarget::None);
  }
  submitSettled();
  emit settledCommitted();
}

void EditorColorTempModel::reset() {
  cctDragMoved_  = false;
  tintDragMoved_ = false;
  if (modeIndex_ == 0 && std::abs(cct_ - asShotCct_) < 1e-6 &&
      std::abs(tint_ - asShotTint_) < 1e-6) {
    setDragActive(false, DragTarget::None);
    return;
  }
  modeIndex_ = 0;
  cct_       = asShotCct_;
  tint_      = asShotTint_;
  setDragActive(false, DragTarget::None);
  emit modeIndexChanged();
  emit cctChanged();
  emit tintChanged();
  submitSettled();
  emit settledCommitted();
}

auto EditorColorTempModel::paramsJson() const -> QString { return buildParamsJson(); }

void EditorColorTempModel::loadFromParams(const QString& mode, double cct, double tint,
                                          bool supported) {
  // Interactive submit echoes AdjustmentSnapshotChanged → loadFromSnapshot while
  // the pointer drag is still open. Aborting drag here turns continuous CCT
  // motion into a single click (updateCctDrag no-ops once dragActive is false).
  if (dragActive_) {
    return;
  }
  supported_ = supported;
  modeIndex_ = ModeIndexFromString(mode);
  // loadFromParams only has one display pair: treat it as the active mode values.
  // When as_shot, also refresh the baseline; when custom, keep prior as-shot cache
  // unless loadFromOperatorParams supplies explicit as_shot_*.
  if (modeIndex_ == 0) {
    asShotCct_  = ClampCct(cct);
    asShotTint_ = ClampTint(tint);
  }
  cct_  = ClampCct(cct);
  tint_ = ClampTint(tint);
  emit supportedChanged();
  emit modeIndexChanged();
  emit asShotCctChanged();
  emit asShotTintChanged();
  emit cctChanged();
  emit tintChanged();
}

void EditorColorTempModel::loadFromOperatorParams(const QVariantMap& params) {
  if (dragActive_) {
    return;
  }
  // Accept either the full GetParams root {"color_temp":{...}} or the inner object.
  QVariantMap ct     = params;
  const auto  nested = params.value(QStringLiteral("color_temp"));
  if (nested.canConvert<QVariantMap>()) {
    ct = nested.toMap();
  }
  if (ct.isEmpty()) {
    return;
  }

  const QString mode = ct.value(QStringLiteral("mode"), QStringLiteral("as_shot")).toString();
  modeIndex_         = ModeIndexFromString(mode);

  // Image-local as-shot baseline (GetParams writes as_shot_*; accept resolved_* legacy).
  auto read_double   = [&](const QString& key, const QString& alt, double fallback) -> double {
    if (ct.contains(key) && ct.value(key).isValid()) {
      return ct.value(key).toDouble();
    }
    if (!alt.isEmpty() && ct.contains(alt) && ct.value(alt).isValid()) {
      return ct.value(alt).toDouble();
    }
    return fallback;
  };

  const double as_shot_cct =
      read_double(QStringLiteral("as_shot_cct"), QStringLiteral("resolved_cct"), asShotCct_);
  const double as_shot_tint =
      read_double(QStringLiteral("as_shot_tint"), QStringLiteral("resolved_tint"), asShotTint_);
  asShotCct_  = ClampCct(as_shot_cct);
  asShotTint_ = ClampTint(as_shot_tint);

  // Display pair: custom mode shows custom_*; as_shot mode shows the baseline.
  if (modeIndex_ == 1) {
    const double custom_cct =
        read_double(QStringLiteral("custom_cct"), QStringLiteral("cct"), cct_);
    const double custom_tint =
        read_double(QStringLiteral("custom_tint"), QStringLiteral("tint"), tint_);
    cct_  = ClampCct(custom_cct);
    tint_ = ClampTint(custom_tint);
  } else {
    // Legacy GetParams sometimes mirrored as-shot into cct/tint only.
    const double display_cct =
        read_double(QStringLiteral("as_shot_cct"), QStringLiteral("cct"), asShotCct_);
    const double display_tint =
        read_double(QStringLiteral("as_shot_tint"), QStringLiteral("tint"), asShotTint_);
    cct_        = ClampCct(display_cct);
    tint_       = ClampTint(display_tint);
    asShotCct_  = cct_;
    asShotTint_ = tint_;
  }

  if (ct.contains(QStringLiteral("supported"))) {
    supported_ = ct.value(QStringLiteral("supported")).toBool();
  }

  emit supportedChanged();
  emit modeIndexChanged();
  emit asShotCctChanged();
  emit asShotTintChanged();
  emit cctChanged();
  emit tintChanged();
}

void EditorColorTempModel::promoteToCustomForEditing() {
  if (modeIndex_ == 1) {
    return;
  }
  modeIndex_ = 1;
  emit modeIndexChanged();
}

void EditorColorTempModel::submitInteractive() { submitNow(buildParamsJson(), false); }

void EditorColorTempModel::submitSettled() { submitNow(buildParamsJson(), true); }

auto EditorColorTempModel::buildParamsJson() const -> QString {
  QJsonObject color_temp;
  color_temp.insert(QStringLiteral("mode"), ModeString(modeIndex_));
  color_temp.insert(QStringLiteral("custom_cct"), ClampCct(cct_));
  color_temp.insert(QStringLiteral("custom_tint"), ClampTint(tint_));
  // Image-local as-shot baseline is always the cached as-shot pair.
  color_temp.insert(QStringLiteral("as_shot_cct"), ClampCct(asShotCct_));
  color_temp.insert(QStringLiteral("as_shot_tint"), ClampTint(asShotTint_));
  QJsonObject root;
  root.insert(QStringLiteral("color_temp"), color_temp);
  return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void EditorColorTempModel::setDragActive(bool active, DragTarget target) {
  if (dragActive_ == active && dragTarget_ == target) {
    return;
  }
  dragActive_ = active;
  dragTarget_ = target;
  emit dragActiveChanged();
}

}  // namespace alcedo::ui
