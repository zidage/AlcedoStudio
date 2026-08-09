//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QColor>
#include <QVariantList>
#include <array>
#include <vector>

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/editor_support/modules/hls.hpp"

namespace alcedo::ui {

/// Phase 6D selective HSL model. Owns eight fixed hue-bin profiles and the
/// active target hue. Submits operator-shaped params matching pipeline_io /
/// ParamsForField(Hls):
///   {"HLS":{"hue_bins":[…],"hls_adj_table":[[h,l,s],…],"h_range_table":[…],
///            "target_hls":[h,0.5,0.5],"hls_adj":[h,l,s],"h_range":…, …}}
/// Selecting a hue swatch only switches the active profile (no submit). Slider
/// edits submit interactive previews and one settled commit on release.
class EditorHlsModel : public EditorAdjustmentModelBase {
  Q_OBJECT
  Q_PROPERTY(
      int activeHueIndex READ activeHueIndex WRITE setActiveHueIndex NOTIFY activeHueIndexChanged)
  Q_PROPERTY(double targetHue READ targetHue NOTIFY activeHueIndexChanged)
  Q_PROPERTY(double hueShift READ hueShift WRITE setHueShift NOTIFY activeProfileChanged)
  Q_PROPERTY(double lightness READ lightness WRITE setLightness NOTIFY activeProfileChanged)
  Q_PROPERTY(double chroma READ chroma WRITE setChroma NOTIFY activeProfileChanged)
  Q_PROPERTY(
      double hueSmoothness READ hueSmoothness WRITE setHueSmoothness NOTIFY activeProfileChanged)
  Q_PROPERTY(QVariantList hueSwatches READ hueSwatches CONSTANT)
  Q_PROPERTY(bool dragActive READ dragActive NOTIFY dragActiveChanged)

 public:
  explicit EditorHlsModel(QObject* parent = nullptr);

  [[nodiscard]] auto                activeHueIndex() const -> int { return activeHueIndex_; }
  /// Plain load setter: switches active bin and loads that profile into the
  /// active sliders. Does not submit.
  void                              setActiveHueIndex(int index);

  [[nodiscard]] auto                targetHue() const -> double;
  [[nodiscard]] auto                hueShift() const -> double { return hueShift_; }
  /// Plain load setter for the active profile. Does not submit.
  void                              setHueShift(double value);
  [[nodiscard]] auto                lightness() const -> double { return lightness_; }
  void                              setLightness(double value);
  [[nodiscard]] auto                chroma() const -> double { return chroma_; }
  void                              setChroma(double value);
  [[nodiscard]] auto                hueSmoothness() const -> double { return hueSmoothness_; }
  void                              setHueSmoothness(double value);

  /// Fixed swatch list for QML: [{index, hue, color}].
  [[nodiscard]] auto                hueSwatches() const -> QVariantList;

  [[nodiscard]] auto                dragActive() const -> bool { return dragActive_; }

  /// User swatch click: save current profile, switch bin, load profile (no submit).
  Q_INVOKABLE void                  selectHueIndex(int index);

  Q_INVOKABLE void                  beginHueShiftDrag();
  Q_INVOKABLE void                  updateHueShiftDrag(double value);
  Q_INVOKABLE void                  finishHueShiftDrag();

  Q_INVOKABLE void                  beginLightnessDrag();
  Q_INVOKABLE void                  updateLightnessDrag(double value);
  Q_INVOKABLE void                  finishLightnessDrag();

  Q_INVOKABLE void                  beginChromaDrag();
  Q_INVOKABLE void                  updateChromaDrag(double value);
  Q_INVOKABLE void                  finishChromaDrag();

  Q_INVOKABLE void                  beginHueSmoothnessDrag();
  Q_INVOKABLE void                  updateHueSmoothnessDrag(double value);
  Q_INVOKABLE void                  finishHueSmoothnessDrag();

  Q_INVOKABLE void                  editHueShift(double value);
  Q_INVOKABLE void                  editLightness(double value);
  Q_INVOKABLE void                  editChroma(double value);
  Q_INVOKABLE void                  editHueSmoothness(double value);
  Q_INVOKABLE void                  commitImmediately();

  /// Reset the active profile field (or all four when field is empty) and settle.
  Q_INVOKABLE void                  resetActiveField(const QString& field);
  Q_INVOKABLE void                  resetAllProfiles();
  [[nodiscard]] Q_INVOKABLE QString paramsJson() const;

  /// Load full tables from snapshot-shaped QVariantMaps without submitting.
  Q_INVOKABLE void                  loadFromTables(const QVariantList& hueAdjTable,
                                                   const QVariantList& hueRangeTable, double targetHue);

 signals:
  void activeHueIndexChanged();
  void activeProfileChanged();
  void dragActiveChanged();
  void settledCommitted();

 private:
  enum class DragTarget { None, HueShift, Lightness, Chroma, HueSmoothness };

  void                 saveActiveProfile();
  void                 loadActiveProfile();
  auto                 applyHueShift(double value) -> bool;
  auto                 applyLightness(double value) -> bool;
  auto                 applyChroma(double value) -> bool;
  auto                 applyHueSmoothness(double value) -> bool;
  void                 beginDrag(DragTarget target);
  void                 finishDrag();
  void                 submitInteractive();
  void                 submitSettled();
  [[nodiscard]] auto   buildParamsJson() const -> QString;

  int                  activeHueIndex_ = 0;
  float                hueShift_       = 0.0f;
  float                lightness_      = 0.0f;
  float                chroma_         = 0.0f;
  float                hueSmoothness_  = hls::kDefaultHueRange;

  hls::HlsProfileArray hueShiftTable_{};
  hls::HlsProfileArray lightnessTable_{};
  hls::HlsProfileArray chromaTable_{};
  hls::HlsProfileArray hueRangeTable_ = hls::MakeFilledArray(hls::kDefaultHueRange);

  bool                 dragActive_    = false;
  DragTarget           dragTarget_    = DragTarget::None;
};

}  // namespace alcedo::ui
