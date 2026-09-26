//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QVariantMap>

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"

namespace alcedo::ui {

/// Color Grade CAT02 white balance on the Look page. Owns temperature (Kelvin) and tint.
/// Uses the RAW White Balance slider scale but has no As Shot / Custom mode: the grade input
/// is treated as balanced to the ACES AP1 white point, which is the default and identity value
/// (about 6000 K). Submits `Cat02WhiteBalanceUpdate` under field key `grade_white_balance`.
/// Load setters do not submit.
class EditorGradeWhiteBalanceModel : public EditorAdjustmentModelBase {
  Q_OBJECT
  Q_PROPERTY(double temperature READ temperature WRITE setTemperature NOTIFY temperatureChanged)
  Q_PROPERTY(double tint READ tint WRITE setTint NOTIFY tintChanged)
  Q_PROPERTY(int temperatureSliderPos READ temperatureSliderPos NOTIFY temperatureChanged)
  Q_PROPERTY(double defaultTemperature READ defaultTemperature CONSTANT)
  Q_PROPERTY(double defaultTint READ defaultTint CONSTANT)
  Q_PROPERTY(bool dragActive READ dragActive NOTIFY dragActiveChanged)

 public:
  explicit EditorGradeWhiteBalanceModel(QObject* parent = nullptr);

  [[nodiscard]] auto temperature() const -> double { return temperature_; }
  /// Plain load setter (Kelvin). Does not submit.
  void               setTemperature(double kelvin);
  [[nodiscard]] auto tint() const -> double { return tint_; }
  /// Plain load setter. Does not submit.
  void               setTint(double value);

  /// Non-linear UI position for the temperature track (color_temp::CctToSliderPos).
  [[nodiscard]] auto temperatureSliderPos() const -> int;
  [[nodiscard]] auto defaultTemperature() const -> double;
  [[nodiscard]] auto defaultTint() const -> double;
  [[nodiscard]] auto dragActive() const -> bool { return dragActive_; }

  Q_INVOKABLE void   beginTemperatureDrag();
  Q_INVOKABLE void   updateTemperatureSliderDrag(int pos);
  Q_INVOKABLE void   finishTemperatureDrag();
  Q_INVOKABLE void   beginTintDrag();
  Q_INVOKABLE void   updateTintDrag(double value);
  Q_INVOKABLE void   finishTintDrag();
  /// Restore the AP1 white temperature and commit one settled transaction.
  Q_INVOKABLE void   resetTemperature();
  /// Restore the AP1 white tint and commit one settled transaction.
  Q_INVOKABLE void   resetTint();

  /// Load from the adjustment snapshot root, its `grade_white_balance` field map, or the inner
  /// {temperature, tint} object. Does not submit. Ignored while a pointer drag is open.
  Q_INVOKABLE void   loadFromSnapshot(const QVariantMap& snapshot);

 signals:
  void temperatureChanged();
  void tintChanged();
  void dragActiveChanged();
  void settledCommitted();

 private:
  enum class DragTarget { None, Temperature, Tint };

  void submitCurrent(bool settled);
  void setDragActive(bool active, DragTarget target);

  double     temperature_;
  double     tint_;
  bool       dragActive_ = false;
  DragTarget dragTarget_ = DragTarget::None;
  /// True once an update changed a value in the open pointer drag. Empty click halves of a
  /// double-click must not settle on finish*.
  bool       dragMoved_  = false;
};

}  // namespace alcedo::ui
