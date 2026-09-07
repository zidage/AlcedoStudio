//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPointF>
#include <QString>
#include <array>
#include <optional>

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/editor_support/cdl_wheel_state.hpp"

namespace alcedo::ui {

/// Phase 6D CDL color-wheel model. Owns Lift / Gamma / Gain disc positions and
/// master offsets, matching the legacy three-trackball layout (Gamma top, Lift
/// and Gain bottom). Submits operator-shaped params matching ParamsForField
/// (ColorWheel):
///   {"color_wheel":{"lift":{…},"gamma":{…},"gain":{…}}}
/// Disc and master pointer drags submit interactive previews and one settled
/// commit on release. Load setters do not submit.
class EditorCdlTrackballModel : public EditorAdjustmentModelBase {
  Q_OBJECT
  Q_PROPERTY(double liftX READ liftX NOTIFY liftChanged)
  Q_PROPERTY(double liftY READ liftY NOTIFY liftChanged)
  Q_PROPERTY(double liftMaster READ liftMaster NOTIFY liftChanged)
  Q_PROPERTY(int liftMasterUi READ liftMasterUi NOTIFY liftChanged)
  Q_PROPERTY(QString liftDeltaText READ liftDeltaText NOTIFY liftChanged)

  Q_PROPERTY(double gammaX READ gammaX NOTIFY gammaChanged)
  Q_PROPERTY(double gammaY READ gammaY NOTIFY gammaChanged)
  Q_PROPERTY(double gammaMaster READ gammaMaster NOTIFY gammaChanged)
  Q_PROPERTY(int gammaMasterUi READ gammaMasterUi NOTIFY gammaChanged)
  Q_PROPERTY(QString gammaDeltaText READ gammaDeltaText NOTIFY gammaChanged)

  Q_PROPERTY(double gainX READ gainX NOTIFY gainChanged)
  Q_PROPERTY(double gainY READ gainY NOTIFY gainChanged)
  Q_PROPERTY(double gainMaster READ gainMaster NOTIFY gainChanged)
  Q_PROPERTY(int gainMasterUi READ gainMasterUi NOTIFY gainChanged)
  Q_PROPERTY(QString gainDeltaText READ gainDeltaText NOTIFY gainChanged)

  Q_PROPERTY(bool dragActive READ dragActive NOTIFY dragActiveChanged)

 public:
  enum class WheelId { Lift = 0, Gamma = 1, Gain = 2 };

  explicit EditorCdlTrackballModel(QObject* parent = nullptr);

  [[nodiscard]] auto                liftX() const -> double { return lift_.disc_position_.x(); }
  [[nodiscard]] auto                liftY() const -> double { return lift_.disc_position_.y(); }
  [[nodiscard]] auto                liftMaster() const -> double { return lift_.master_offset_; }
  [[nodiscard]] auto                liftMasterUi() const -> int;
  [[nodiscard]] auto                liftDeltaText() const -> QString;

  [[nodiscard]] auto                gammaX() const -> double { return gamma_.disc_position_.x(); }
  [[nodiscard]] auto                gammaY() const -> double { return gamma_.disc_position_.y(); }
  [[nodiscard]] auto                gammaMaster() const -> double { return gamma_.master_offset_; }
  [[nodiscard]] auto                gammaMasterUi() const -> int;
  [[nodiscard]] auto                gammaDeltaText() const -> QString;

  [[nodiscard]] auto                gainX() const -> double { return gain_.disc_position_.x(); }
  [[nodiscard]] auto                gainY() const -> double { return gain_.disc_position_.y(); }
  [[nodiscard]] auto                gainMaster() const -> double { return gain_.master_offset_; }
  [[nodiscard]] auto                gainMasterUi() const -> int;
  [[nodiscard]] auto                gainDeltaText() const -> QString;

  [[nodiscard]] auto                dragActive() const -> bool { return dragActive_; }

  /// Plain load setters. Do not submit.
  Q_INVOKABLE void                  setWheelDisc(const QString& wheel, double x, double y);
  Q_INVOKABLE void                  setWheelMaster(const QString& wheel, double master);
  Q_INVOKABLE void                  setWheelMasterUi(const QString& wheel, int ui_value);

  Q_INVOKABLE void                  beginDiscDrag(const QString& wheel);
  Q_INVOKABLE void                  updateDiscDrag(const QString& wheel, double x, double y);
  Q_INVOKABLE void                  finishDiscDrag();

  Q_INVOKABLE void                  beginMasterDrag(const QString& wheel);
  Q_INVOKABLE void                  updateMasterDragUi(const QString& wheel, int ui_value);
  Q_INVOKABLE void                  finishMasterDrag();

  /// Double-click / reset affordance for one wheel (disc + master).
  Q_INVOKABLE void                  resetWheel(const QString& wheel);
  Q_INVOKABLE void                  resetAll();
  [[nodiscard]] Q_INVOKABLE QString paramsJson() const;
  [[nodiscard]] Q_INVOKABLE QString wheelDeltaText(const QString& wheel) const;

  /// C++ load path used by tests and snapshot reload.
  void setWheels(const CdlWheelState& lift, const CdlWheelState& gamma, const CdlWheelState& gain);
  [[nodiscard]] auto liftWheel() const -> const CdlWheelState& { return lift_; }
  [[nodiscard]] auto gammaWheel() const -> const CdlWheelState& { return gamma_; }
  [[nodiscard]] auto gainWheel() const -> const CdlWheelState& { return gain_; }

 signals:
  void liftChanged();
  void gammaChanged();
  void gainChanged();
  void dragActiveChanged();
  void settledCommitted();

 private:
  [[nodiscard]] static auto ParseWheelId(const QString& wheel) -> std::optional<WheelId>;
  [[nodiscard]] auto        wheelState(WheelId id) -> CdlWheelState&;
  [[nodiscard]] auto        wheelState(WheelId id) const -> const CdlWheelState&;
  void                      recomputeDerived(WheelId id);
  void                      emitWheel(WheelId id);
  void                      applyDisc(WheelId id, double x, double y);
  void                      applyMasterUi(WheelId id, int ui_value);
  void                      submitInteractive();
  void                      submitSettled();
  [[nodiscard]] auto        currentColorWheelWrite() const -> alcedo::ColorWheelUpdate;
  [[nodiscard]] auto        buildParamsJson() const -> QString;
  [[nodiscard]] static auto addUnity(WheelId id) -> bool;
  [[nodiscard]] static auto invertDelta(WheelId id) -> bool;

  CdlWheelState             lift_           = DefaultLiftWheelState();
  CdlWheelState             gamma_          = DefaultGammaGainWheelState();
  CdlWheelState             gain_           = DefaultGammaGainWheelState();

  bool                      dragActive_     = false;
  WheelId                   dragWheel_      = WheelId::Lift;
  bool                      draggingDisc_   = false;
  bool                      draggingMaster_ = false;
  /// True once updateDisc/Master changed state in the open gesture.
  bool                      dragMoved_      = false;
};

}  // namespace alcedo::ui
