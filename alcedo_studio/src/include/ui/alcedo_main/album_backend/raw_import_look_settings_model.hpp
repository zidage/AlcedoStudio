//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QList>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QVariantList>
#include <string>

#include "app/raw_import_look_settings.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"

namespace alcedo::ui {

/**
 * @brief Settings page model of the RAW import look (#920): the RAW Import section of
 *        Settings > Default Behavior.
 *
 * Owns one @ref alcedo::RawImportLookSettings and the pending values of the six rows. The page
 * edits the pending values; @ref apply writes them through the setters of the settings. Values are
 * in the units of the page:
 *
 * | Property | Unit and range | App default |
 * | --- | --- | --- |
 * | `exposureEv` | EV, -10 to +10, rounded to 0.01 | +1.50 |
 * | `saturation` | editor Saturation value, -100 to +100, rounded to 1 | +30 |
 * | `toneCurvePreset` | id of a Tone Curve preset (`alcedo::ToneCurvePresets`) | `cl_digital` |
 * | `drtMethod` | `aces_2_0` or `open_drt` | `open_drt` |
 * | `openDrtLookPreset` | one of `alcedo::RawImportOpenDrtLookPresets` | `standard` |
 * | `lensCorrection` | bool | false |
 *
 * The app default of each row is the value of a value-initialized `alcedo::RawImportLook`.
 *
 * Load: @ref load reads the stored look (`RawImportLookSettings::Read`), so a value written by
 * another window or by `alcedo-cli` appears. It sets the pending values and the loaded values.
 *
 * Edit: a setter that gets a value outside the table rejects it, keeps the previous pending
 * value, sets @ref errorMessage, and returns false. A row reset and @ref resetAll set the pending
 * values to the app defaults.
 *
 * Apply: @ref apply writes each row whose pending value differs from its loaded value. A row whose
 * pending value equals its app default is written as `RawImportLookSettings::Reset`, so the
 * settings file keeps only the values that the user changed. Rows that the page did not change
 * are not written, so a value that another writer stored after @ref load stays.
 *
 * The display names of the presets and methods are translated in the context of this class and
 * follow a change of the application language.
 *
 * Threading: GUI thread only. Side effects: @ref apply writes the settings store.
 */
class RawImportLookSettingsModel final : public QObject {
  Q_OBJECT
  Q_PROPERTY(double exposureEv READ exposure_ev NOTIFY pendingChanged)
  Q_PROPERTY(double saturation READ saturation NOTIFY pendingChanged)
  Q_PROPERTY(QString toneCurvePreset READ tone_curve_preset NOTIFY pendingChanged)
  Q_PROPERTY(QString drtMethod READ drt_method NOTIFY pendingChanged)
  Q_PROPERTY(QString openDrtLookPreset READ open_drt_look_preset NOTIFY pendingChanged)
  Q_PROPERTY(bool lensCorrection READ lens_correction NOTIFY pendingChanged)

  /// True for a row whose pending value differs from its app default.
  Q_PROPERTY(bool exposureEvModified READ exposure_ev_modified NOTIFY pendingChanged)
  Q_PROPERTY(bool saturationModified READ saturation_modified NOTIFY pendingChanged)
  Q_PROPERTY(bool toneCurvePresetModified READ tone_curve_preset_modified NOTIFY pendingChanged)
  Q_PROPERTY(bool drtMethodModified READ drt_method_modified NOTIFY pendingChanged)
  Q_PROPERTY(
      bool openDrtLookPresetModified READ open_drt_look_preset_modified NOTIFY pendingChanged)
  Q_PROPERTY(bool lensCorrectionModified READ lens_correction_modified NOTIFY pendingChanged)
  /// True when any row differs from its app default.
  Q_PROPERTY(bool anyModified READ any_modified NOTIFY pendingChanged)
  /// App defaults of the slider rows, in page units (the default mark of the slider).
  Q_PROPERTY(double exposureEvDefault READ exposure_ev_default CONSTANT)
  Q_PROPERTY(double saturationDefault READ saturation_default CONSTANT)

  /// The Tone Curve presets in menu order: maps with `value` (id), `label` (translated name), and
  /// `group` (translated menu group; empty for Linear).
  Q_PROPERTY(QVariantList toneCurvePresets READ tone_curve_presets NOTIFY labelsChanged)
  /// The DRT methods: maps with `value` (`aces_2_0`, `open_drt`) and `label`.
  Q_PROPERTY(QVariantList drtMethods READ drt_methods NOTIFY labelsChanged)
  /// The OpenDRT look presets: maps with `value` (preset id) and `label`.
  Q_PROPERTY(QVariantList openDrtLookPresets READ open_drt_look_presets NOTIFY labelsChanged)

  /// The RGB curve of the pending preset on the curve axis: the line of the editor curve
  /// (`curve::SampleCurveLine` of `curve::SolveCurveSpline`).
  Q_PROPERTY(QList<QPointF> toneCurveLine READ tone_curve_line NOTIFY pendingChanged)
  /// The control points of the pending preset.
  Q_PROPERTY(QList<QPointF> toneCurvePoints READ tone_curve_points NOTIFY pendingChanged)
  /// 18% grey and one stop on the curve axis, for the EV grid of the preview.
  Q_PROPERTY(double axisGrey READ axis_grey CONSTANT)
  Q_PROPERTY(double axisStop READ axis_stop CONSTANT)

  /// Message of the last rejected value or failed write; empty after an accepted edit.
  Q_PROPERTY(QString errorMessage READ error_message NOTIFY errorMessageChanged)

 public:
  static constexpr double kMinExposureEv      = -10.0;
  static constexpr double kMaxExposureEv      = 10.0;
  static constexpr double kMinSaturation      = -100.0;
  static constexpr double kMaxSaturation      = 100.0;
  static constexpr auto   kAces20MethodValue  = "aces_2_0";
  static constexpr auto   kOpenDrtMethodValue = "open_drt";

  /// @param settings_file INI file of the settings; empty selects the application QSettings.
  explicit RawImportLookSettingsModel(QString settings_file = {}, QObject* parent = nullptr);

  [[nodiscard]] auto        exposure_ev() const -> double { return pending_.exposure_ev; }
  [[nodiscard]] auto        saturation() const -> double { return pending_.saturation; }
  [[nodiscard]] auto        tone_curve_preset() const -> QString;
  [[nodiscard]] auto        drt_method() const -> QString;
  [[nodiscard]] auto        open_drt_look_preset() const -> QString;
  [[nodiscard]] auto        lens_correction() const -> bool { return pending_.lens_correction; }

  [[nodiscard]] auto        exposure_ev_modified() const -> bool;
  [[nodiscard]] auto        saturation_modified() const -> bool;
  [[nodiscard]] auto        tone_curve_preset_modified() const -> bool;
  [[nodiscard]] auto        drt_method_modified() const -> bool;
  [[nodiscard]] auto        open_drt_look_preset_modified() const -> bool;
  [[nodiscard]] auto        lens_correction_modified() const -> bool;
  [[nodiscard]] auto        any_modified() const -> bool;
  [[nodiscard]] static auto exposure_ev_default() -> double;
  [[nodiscard]] static auto saturation_default() -> double;
  /// True when the pending value of @p field differs from its app default.
  [[nodiscard]] auto        IsModified(alcedo::RawImportLookField field) const -> bool;

  [[nodiscard]] static auto tone_curve_presets() -> QVariantList;
  [[nodiscard]] static auto drt_methods() -> QVariantList;
  [[nodiscard]] static auto open_drt_look_presets() -> QVariantList;
  [[nodiscard]] auto        tone_curve_line() const -> QList<QPointF> { return curve_line_; }
  [[nodiscard]] auto        tone_curve_points() const -> QList<QPointF> { return curve_points_; }
  [[nodiscard]] static auto axis_grey() -> double;
  [[nodiscard]] static auto axis_stop() -> double;
  [[nodiscard]] auto        error_message() const -> QString { return error_message_; }

  /// Read the stored look into the pending and the loaded values. Clears @ref errorMessage.
  Q_INVOKABLE void          load();

  /// Pending setters. Each returns false, keeps the previous value, and sets @ref errorMessage
  /// for a value outside the table.
  Q_INVOKABLE bool          setExposureEv(double exposure_ev);
  Q_INVOKABLE bool          setSaturation(double saturation);
  Q_INVOKABLE bool          setToneCurvePreset(const QString& preset_id);
  Q_INVOKABLE bool          setDrtMethod(const QString& method);
  Q_INVOKABLE bool          setOpenDrtLookPreset(const QString& preset);
  Q_INVOKABLE void          setLensCorrection(bool enabled);

  /// Set the pending value of the row @p row (a property name above, such as `exposureEv`) to
  /// its app default. Returns false for an unknown row name.
  Q_INVOKABLE bool          resetRow(const QString& row);
  /// Set the pending value of @p field to its app default.
  void                      ResetField(alcedo::RawImportLookField field);
  /// Set every pending value to its app default.
  Q_INVOKABLE void          resetAll();

  /**
   * @brief Write the changed rows to the settings store and read the stored look again.
   * @return false when a write fails; @ref errorMessage names the failure. Rows written before the
   *         failure stay written.
   */
  Q_INVOKABLE bool          apply();

 signals:
  void pendingChanged();
  void labelsChanged();
  void errorMessageChanged();

 private:
  /// The values of the six rows in page units. The model keeps two: the pending values that the
  /// page edits, and the values that the last @ref load read, which decide the rows that
  /// @ref apply writes. Both live as long as the model and are replaced by @ref load.
  struct RowValues {
    double      exposure_ev = 0.0;
    double      saturation  = 0.0;
    std::string tone_curve_preset;
    DrtMethod   drt_method = DrtMethod::OpenDrt;
    std::string open_drt_look_preset;
    bool        lens_correction = false;
  };

  [[nodiscard]] static auto     RowValuesOf(const alcedo::RawImportLook& look) -> RowValues;
  [[nodiscard]] static auto     AppDefaults() -> const RowValues&;
  [[nodiscard]] static auto     FieldEqual(alcedo::RawImportLookField field, const RowValues& lhs,
                                           const RowValues& rhs) -> bool;
  void                          WriteField(alcedo::RawImportLookField field);
  void                          RebuildCurve();
  void                          SetErrorMessage(QString message);
  /// Rejects an edit: keeps the pending value, names the error, and returns false.
  auto                          Reject(QString message) -> bool;
  /// Accepts an edit: clears the error and notifies the page.
  void                          Accepted(bool curve_changed);

  alcedo::RawImportLookSettings settings_;
  RowValues                     pending_;
  RowValues                     loaded_;
  QList<QPointF>                curve_line_;
  QList<QPointF>                curve_points_;
  QString                       error_message_;
};

}  // namespace alcedo::ui
