//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QSettings>
#include <QString>

#include <memory>
#include <string_view>

#include "app/raw_import_look_source.hpp"
#include "edit/graph/drt_node_model.hpp"

namespace alcedo {

/// One value of the RAW import look in the settings.
enum class RawImportLookField {
  ExposureEv,
  Saturation,
  ToneCurvePreset,
  DrtMethod,
  OpenDrtLookPreset,
  LensCorrection,
};

/**
 * @brief The RAW import look in the application QSettings (#918).
 *
 * Owns the keys under `import/look/`:
 *
 * | Key | Stored value | Product Default |
 * | --- | --- | --- |
 * | `exposureEv` | Exposure in EV, -10 to 10 | 1.5 |
 * | `saturationPercent` | Editor Saturation value, -100 to 100 (multiplier 1 + value / 100) | 30 |
 * | `toneCurvePreset` | Tone Curve preset id | `cl_digital` |
 * | `drtMethod` | `aces_2_0` or `open_drt` | `open_drt` |
 * | `openDrtLookPreset` | One of @ref RawImportOpenDrtLookPresets | `standard` |
 * | `lensCorrection` | `true` or `false` | `false` |
 *
 * A missing key gives the product Default of that value. A stored value that the table does not
 * allow also gives the product Default, so a settings file of another version or a hand edit
 * never stops an import. The setters write one key each and reject a value outside the table;
 * the resets remove keys.
 *
 * Threading: each call opens its own QSettings, so @ref Read can run on any thread. Side effects:
 * the setters and resets write the settings store.
 */
class RawImportLookSettings final : public RawImportLookSource {
 public:
  /// @param settings_file INI file of the settings; empty selects the application QSettings.
  explicit RawImportLookSettings(QString settings_file = {});

  [[nodiscard]] auto Read() const -> RawImportLook override;

  /// @throws std::invalid_argument when @p exposure_ev is outside [-10, 10] or not finite.
  void               SetExposureEv(double exposure_ev);
  /// @param percent Editor Saturation value: +30 is the multiplier 1.3.
  /// @throws std::invalid_argument when @p percent is outside [-100, 100] or not finite.
  void               SetSaturationPercent(double percent);
  /// @throws std::invalid_argument when @p preset_id names no Tone Curve preset.
  void               SetToneCurvePreset(std::string_view preset_id);
  /// @throws std::invalid_argument when @p method is not ACES 2.0 or OpenDRT.
  void               SetDrtMethod(DrtMethod method);
  /// @throws std::invalid_argument when @p preset is not in @ref RawImportOpenDrtLookPresets.
  void               SetOpenDrtLookPreset(std::string_view preset);
  void               SetLensCorrection(bool enabled);

  /// Remove the key of @p field: the next @ref Read gives its product Default.
  void               Reset(RawImportLookField field);
  /// Remove every key of the look.
  void               ResetAll();

  /// The QSettings key of @p field, for example `import/look/exposureEv`.
  [[nodiscard]] static auto Key(RawImportLookField field) -> QString;

 private:
  [[nodiscard]] auto Settings() const -> std::unique_ptr<QSettings>;

  QString settings_file_;
};

}  // namespace alcedo
