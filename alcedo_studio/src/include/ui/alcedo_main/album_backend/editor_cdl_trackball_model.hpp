//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPointF>
#include <QString>
#include <array>
#include <initializer_list>
#include <optional>

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/editor_support/cdl_wheel_state.hpp"

namespace alcedo::ui {

/// Tone color wheel panel model. Owns the Lift / Gamma / Gain / Offset disc positions and Master
/// values (stops). Submits operator-shaped params matching ParamsForField (ColorWheel):
///   {"color_wheel":{"version":2,"lift":{…},"gamma":{…},"gain":{…},"offset":{…}}}
/// Disc and Master pointer drags submit interactive previews and one settled commit on release;
/// a drag writes only its own wheel. Load setters do not submit.
class EditorCdlTrackballModel : public EditorAdjustmentModelBase {
  Q_OBJECT
  Q_PROPERTY(double liftX READ liftX NOTIFY liftChanged)
  Q_PROPERTY(double liftY READ liftY NOTIFY liftChanged)
  Q_PROPERTY(double liftMaster READ liftMaster NOTIFY liftChanged)
  Q_PROPERTY(QString liftDeltaText READ liftDeltaText NOTIFY liftChanged)

  Q_PROPERTY(double gammaX READ gammaX NOTIFY gammaChanged)
  Q_PROPERTY(double gammaY READ gammaY NOTIFY gammaChanged)
  Q_PROPERTY(double gammaMaster READ gammaMaster NOTIFY gammaChanged)
  Q_PROPERTY(QString gammaDeltaText READ gammaDeltaText NOTIFY gammaChanged)

  Q_PROPERTY(double gainX READ gainX NOTIFY gainChanged)
  Q_PROPERTY(double gainY READ gainY NOTIFY gainChanged)
  Q_PROPERTY(double gainMaster READ gainMaster NOTIFY gainChanged)
  Q_PROPERTY(QString gainDeltaText READ gainDeltaText NOTIFY gainChanged)

  Q_PROPERTY(double offsetX READ offsetX NOTIFY offsetChanged)
  Q_PROPERTY(double offsetY READ offsetY NOTIFY offsetChanged)
  Q_PROPERTY(double offsetMaster READ offsetMaster NOTIFY offsetChanged)
  Q_PROPERTY(QString offsetDeltaText READ offsetDeltaText NOTIFY offsetChanged)

  Q_PROPERTY(bool dragActive READ dragActive NOTIFY dragActiveChanged)

 public:
  enum class WheelId { Lift = 0, Gamma = 1, Gain = 2, Offset = 3 };

  explicit EditorCdlTrackballModel(QObject* parent = nullptr);

  [[nodiscard]] auto liftX() const -> double { return wheel(WheelId::Lift).disc_position_.x(); }
  [[nodiscard]] auto liftY() const -> double { return wheel(WheelId::Lift).disc_position_.y(); }
  [[nodiscard]] auto liftMaster() const -> double { return wheel(WheelId::Lift).master_offset_; }
  [[nodiscard]] auto liftDeltaText() const -> QString;

  [[nodiscard]] auto gammaX() const -> double { return wheel(WheelId::Gamma).disc_position_.x(); }
  [[nodiscard]] auto gammaY() const -> double { return wheel(WheelId::Gamma).disc_position_.y(); }
  [[nodiscard]] auto gammaMaster() const -> double { return wheel(WheelId::Gamma).master_offset_; }
  [[nodiscard]] auto gammaDeltaText() const -> QString;

  [[nodiscard]] auto gainX() const -> double { return wheel(WheelId::Gain).disc_position_.x(); }
  [[nodiscard]] auto gainY() const -> double { return wheel(WheelId::Gain).disc_position_.y(); }
  [[nodiscard]] auto gainMaster() const -> double { return wheel(WheelId::Gain).master_offset_; }
  [[nodiscard]] auto gainDeltaText() const -> QString;

  [[nodiscard]] auto offsetX() const -> double { return wheel(WheelId::Offset).disc_position_.x(); }
  [[nodiscard]] auto offsetY() const -> double { return wheel(WheelId::Offset).disc_position_.y(); }
  [[nodiscard]] auto offsetMaster() const -> double {
    return wheel(WheelId::Offset).master_offset_;
  }
  [[nodiscard]] auto                offsetDeltaText() const -> QString;

  [[nodiscard]] auto                dragActive() const -> bool { return dragActive_; }

  /// Disc position of @p wheel, for the trackball item.
  [[nodiscard]] auto                discPosition(const QString& wheel) const -> QPointF;

  /// Plain load setters. Do not submit. @p master is in stops.
  Q_INVOKABLE void                  setWheelDisc(const QString& wheel, double x, double y);
  Q_INVOKABLE void                  setWheelMaster(const QString& wheel, double master);

  Q_INVOKABLE void                  beginDiscDrag(const QString& wheel);
  Q_INVOKABLE void                  updateDiscDrag(const QString& wheel, double x, double y);
  Q_INVOKABLE void                  finishDiscDrag();

  Q_INVOKABLE void                  beginMasterDrag(const QString& wheel);
  /// @p master is in stops, clamped to the Master range.
  Q_INVOKABLE void                  updateMasterDrag(const QString& wheel, double master);
  Q_INVOKABLE void                  finishMasterDrag();

  /// Double-click / reset affordance for one wheel (disc + master).
  Q_INVOKABLE void                  resetWheel(const QString& wheel);
  Q_INVOKABLE void                  resetAll();
  [[nodiscard]] Q_INVOKABLE QString paramsJson() const;
  [[nodiscard]] Q_INVOKABLE QString wheelDeltaText(const QString& wheel) const;

  [[nodiscard]] auto                wheel(WheelId id) const -> const CdlWheelState& {
    return wheels_[static_cast<std::size_t>(id)];
  }

 signals:
  void liftChanged();
  void gammaChanged();
  void gainChanged();
  void offsetChanged();
  void dragActiveChanged();
  void settledCommitted();

 private:
  [[nodiscard]] static auto ParseWheelId(const QString& wheel) -> std::optional<WheelId>;
  [[nodiscard]] auto        wheelState(WheelId id) -> CdlWheelState&;
  void                      recomputeDerived(WheelId id);
  void                      emitWheel(WheelId id);
  void                      clearDrag();
  void                      applyDisc(WheelId id, double x, double y);
  void                      applyMaster(WheelId id, double master);
  void                      submitWheels(std::initializer_list<WheelId> ids, bool settled);
  [[nodiscard]] auto        colorWheelWrite(std::initializer_list<WheelId> ids) const
      -> alcedo::ColorWheelUpdate;

  std::array<CdlWheelState, 4> wheels_{};

  bool                         dragActive_     = false;
  WheelId                      dragWheel_      = WheelId::Lift;
  bool                         draggingDisc_   = false;
  bool                         draggingMaster_ = false;
  /// True once updateDisc/Master changed state in the open drag.
  bool                         dragMoved_      = false;
};

}  // namespace alcedo::ui
