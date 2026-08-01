//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"

namespace alcedo::ui {

/// Phase 6D white-balance model. Owns As Shot / Custom mode, CCT, and tint.
/// Submits operator-shaped params through `IEditorAdjustmentSubmitter`:
///   {"color_temp":{"mode":"as_shot"|"custom","custom_cct":…,"custom_tint":…,
///                  "as_shot_cct":…,"as_shot_tint":…}}
/// Editing CCT or tint promotes mode to Custom. Reset restores As Shot.
/// Load setters do not submit.
class EditorColorTempModel : public EditorAdjustmentModelBase {
  Q_OBJECT
  Q_PROPERTY(int modeIndex READ modeIndex WRITE setModeIndex NOTIFY modeIndexChanged)
  Q_PROPERTY(QString modeValue READ modeValue NOTIFY modeIndexChanged)
  Q_PROPERTY(double cct READ cct WRITE setCct NOTIFY cctChanged)
  Q_PROPERTY(double tint READ tint WRITE setTint NOTIFY tintChanged)
  Q_PROPERTY(double asShotCct READ asShotCct WRITE setAsShotCct NOTIFY asShotCctChanged)
  Q_PROPERTY(double asShotTint READ asShotTint WRITE setAsShotTint NOTIFY asShotTintChanged)
  Q_PROPERTY(bool supported READ supported WRITE setSupported NOTIFY supportedChanged)
  Q_PROPERTY(int cctSliderPos READ cctSliderPos WRITE setCctSliderPos NOTIFY cctChanged)
  Q_PROPERTY(bool dragActive READ dragActive NOTIFY dragActiveChanged)

 public:
  explicit EditorColorTempModel(QObject* parent = nullptr);

  [[nodiscard]] auto modeIndex() const -> int { return modeIndex_; }
  /// Plain load setter. Does not submit.
  void setModeIndex(int index);
  [[nodiscard]] auto modeValue() const -> QString;

  [[nodiscard]] auto cct() const -> double { return cct_; }
  /// Plain load setter (Kelvin). Does not submit.
  void setCct(double kelvin);
  [[nodiscard]] auto tint() const -> double { return tint_; }
  /// Plain load setter. Does not submit.
  void setTint(double value);

  [[nodiscard]] auto asShotCct() const -> double { return asShotCct_; }
  void setAsShotCct(double kelvin);
  [[nodiscard]] auto asShotTint() const -> double { return asShotTint_; }
  void setAsShotTint(double value);

  [[nodiscard]] auto supported() const -> bool { return supported_; }
  void setSupported(bool supported);

  /// Non-linear UI position for the CCT track (matches color_temp::CctToSliderPos).
  [[nodiscard]] auto cctSliderPos() const -> int;
  /// Plain load via slider position. Does not submit.
  void setCctSliderPos(int pos);

  [[nodiscard]] auto dragActive() const -> bool { return dragActive_; }

  /// User mode selection: commits one settled transaction.
  Q_INVOKABLE void selectMode(int index);
  Q_INVOKABLE void beginCctDrag();
  Q_INVOKABLE void updateCctDrag(double kelvin);
  Q_INVOKABLE void updateCctSliderDrag(int pos);
  Q_INVOKABLE void finishCctDrag();
  Q_INVOKABLE void beginTintDrag();
  Q_INVOKABLE void updateTintDrag(double value);
  Q_INVOKABLE void finishTintDrag();
  Q_INVOKABLE void editCct(double kelvin);
  Q_INVOKABLE void editTint(double value);
  Q_INVOKABLE void commitImmediately();
  /// Restore As Shot and commit one settled transaction.
  Q_INVOKABLE void reset();
  [[nodiscard]] Q_INVOKABLE QString paramsJson() const;

  /// Snapshot load helper: mode string ("as_shot"/"custom"), CCT, tint, supported.
  Q_INVOKABLE void loadFromParams(const QString& mode, double cct, double tint, bool supported);

 signals:
  void modeIndexChanged();
  void cctChanged();
  void tintChanged();
  void asShotCctChanged();
  void asShotTintChanged();
  void supportedChanged();
  void dragActiveChanged();
  void settledCommitted();

 private:
  enum class DragTarget { None, Cct, Tint };

  void promoteToCustomForEditing();
  void submitInteractive();
  void submitSettled();
  [[nodiscard]] auto buildParamsJson() const -> QString;
  void setDragActive(bool active, DragTarget target);

  int        modeIndex_   = 0;  // 0 = as_shot, 1 = custom
  double     cct_         = 6500.0;
  double     tint_        = 0.0;
  double     asShotCct_   = 6500.0;
  double     asShotTint_  = 0.0;
  bool       supported_   = true;
  bool       dragActive_  = false;
  DragTarget dragTarget_  = DragTarget::None;
  /// True once updateCct*/updateTint* changed a value in the open gesture.
  /// Empty click halves of a double-click must not settle on finish*.
  bool cctDragMoved_  = false;
  bool tintDragMoved_ = false;
};

}  // namespace alcedo::ui
