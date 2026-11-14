//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/raw_import_look_settings.hpp"

#include <QMetaType>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "edit/operators/models/tone_curve_presets.hpp"

namespace alcedo {
namespace {

constexpr double kMinSaturationPercent = -100.0;
constexpr double kMaxSaturationPercent = 100.0;

constexpr std::string_view kAces20MethodText  = "aces_2_0";
constexpr std::string_view kOpenDrtMethodText = "open_drt";

constexpr RawImportLookField kAllFields[] = {
    RawImportLookField::ExposureEv,      RawImportLookField::Saturation,
    RawImportLookField::ToneCurvePreset, RawImportLookField::DrtMethod,
    RawImportLookField::OpenDrtLookPreset, RawImportLookField::LensCorrection,
};

auto InClosedRange(double value, double min, double max) -> bool {
  return std::isfinite(value) && value >= min && value <= max;
}

/// The editor Saturation value as the multiplier of the Saturation Model. The sum runs in double,
/// so +30 gives the float nearest 1.3, the value of @ref kDefaultPipelineSaturation.
auto SaturationMultiplier(double percent) -> float {
  return static_cast<float>(1.0 + percent / 100.0);
}

auto ToText(const QVariant& value) -> std::optional<std::string> {
  if (value.metaType().id() != QMetaType::QString) return std::nullopt;
  return value.toString().toStdString();
}

auto ToNumber(const QVariant& value) -> std::optional<double> {
  bool       ok     = false;
  const auto number = value.toDouble(&ok);
  if (!ok) return std::nullopt;
  return number;
}

/// An INI file stores a bool as the text `true` or `false`; the native store keeps the bool.
auto ToBool(const QVariant& value) -> std::optional<bool> {
  if (value.metaType().id() == QMetaType::Bool) return value.toBool();
  const auto text = ToText(value);
  if (text == "true") return true;
  if (text == "false") return false;
  return std::nullopt;
}

auto MethodFromText(std::string_view text) -> std::optional<DrtMethod> {
  if (text == kAces20MethodText) return DrtMethod::Aces20;
  if (text == kOpenDrtMethodText) return DrtMethod::OpenDrt;
  return std::nullopt;
}

auto IsOpenDrtLookPreset(std::string_view preset) -> bool {
  const auto presets = RawImportOpenDrtLookPresets();
  return std::ranges::find(presets, preset) != presets.end();
}

}  // namespace

RawImportLookSettings::RawImportLookSettings(QString settings_file)
    : settings_file_(std::move(settings_file)) {}

auto RawImportLookSettings::Settings() const -> std::unique_ptr<QSettings> {
  if (settings_file_.isEmpty()) return std::make_unique<QSettings>();
  return std::make_unique<QSettings>(settings_file_, QSettings::IniFormat);
}

auto RawImportLookSettings::Key(RawImportLookField field) -> QString {
  switch (field) {
    case RawImportLookField::ExposureEv:
      return QStringLiteral("import/look/exposureEv");
    case RawImportLookField::Saturation:
      return QStringLiteral("import/look/saturationPercent");
    case RawImportLookField::ToneCurvePreset:
      return QStringLiteral("import/look/toneCurvePreset");
    case RawImportLookField::DrtMethod:
      return QStringLiteral("import/look/drtMethod");
    case RawImportLookField::OpenDrtLookPreset:
      return QStringLiteral("import/look/openDrtLookPreset");
    case RawImportLookField::LensCorrection:
      return QStringLiteral("import/look/lensCorrection");
  }
  throw std::invalid_argument("Unknown RAW import look field");
}

auto RawImportLookSettings::Read() const -> RawImportLook {
  const auto    settings = Settings();
  RawImportLook look;
  const auto    stored   = [&](RawImportLookField field) -> std::optional<QVariant> {
    const auto key = Key(field);
    if (!settings->contains(key)) return std::nullopt;
    return settings->value(key);
  };

  if (const auto value = stored(RawImportLookField::ExposureEv)) {
    const auto number = ToNumber(*value);
    if (number && InClosedRange(*number, kRawImportLookMinExposureEv,
                                kRawImportLookMaxExposureEv)) {
      look.exposure_ev = static_cast<float>(*number);
    }
  }
  if (const auto value = stored(RawImportLookField::Saturation)) {
    const auto number = ToNumber(*value);
    if (number && InClosedRange(*number, kMinSaturationPercent, kMaxSaturationPercent)) {
      look.saturation = SaturationMultiplier(*number);
    }
  }
  if (const auto value = stored(RawImportLookField::ToneCurvePreset)) {
    const auto text = ToText(*value);
    if (text && FindToneCurvePreset(*text) != nullptr) look.tone_curve_preset_id = *text;
  }
  if (const auto value = stored(RawImportLookField::DrtMethod)) {
    const auto text   = ToText(*value);
    const auto method = text ? MethodFromText(*text) : std::nullopt;
    if (method) look.drt_method = *method;
  }
  if (const auto value = stored(RawImportLookField::OpenDrtLookPreset)) {
    const auto text = ToText(*value);
    if (text && IsOpenDrtLookPreset(*text)) look.open_drt_look_preset = *text;
  }
  if (const auto value = stored(RawImportLookField::LensCorrection)) {
    if (const auto enabled = ToBool(*value)) look.lens_correction = *enabled;
  }
  return look;
}

void RawImportLookSettings::SetExposureEv(double exposure_ev) {
  if (!InClosedRange(exposure_ev, kRawImportLookMinExposureEv, kRawImportLookMaxExposureEv)) {
    throw std::invalid_argument("RAW import look exposure must be in [-10, 10] EV");
  }
  Settings()->setValue(Key(RawImportLookField::ExposureEv), exposure_ev);
}

void RawImportLookSettings::SetSaturationPercent(double percent) {
  if (!InClosedRange(percent, kMinSaturationPercent, kMaxSaturationPercent)) {
    throw std::invalid_argument("RAW import look saturation must be in [-100, 100]");
  }
  Settings()->setValue(Key(RawImportLookField::Saturation), percent);
}

void RawImportLookSettings::SetToneCurvePreset(std::string_view preset_id) {
  if (FindToneCurvePreset(preset_id) == nullptr) {
    throw std::invalid_argument("RAW import look names an unknown Tone Curve preset: " +
                                std::string{preset_id});
  }
  Settings()->setValue(Key(RawImportLookField::ToneCurvePreset),
                       QString::fromUtf8(preset_id.data(), static_cast<qsizetype>(preset_id.size())));
}

void RawImportLookSettings::SetDrtMethod(DrtMethod method) {
  std::string_view text;
  switch (method) {
    case DrtMethod::Aces20:
      text = kAces20MethodText;
      break;
    case DrtMethod::OpenDrt:
      text = kOpenDrtMethodText;
      break;
    default:
      throw std::invalid_argument("RAW import look DRT method must be ACES 2.0 or OpenDRT");
  }
  Settings()->setValue(Key(RawImportLookField::DrtMethod),
                       QString::fromLatin1(text.data(), static_cast<qsizetype>(text.size())));
}

void RawImportLookSettings::SetOpenDrtLookPreset(std::string_view preset) {
  if (!IsOpenDrtLookPreset(preset)) {
    throw std::invalid_argument("RAW import look names an unknown OpenDRT look preset: " +
                                std::string{preset});
  }
  Settings()->setValue(Key(RawImportLookField::OpenDrtLookPreset),
                       QString::fromLatin1(preset.data(), static_cast<qsizetype>(preset.size())));
}

void RawImportLookSettings::SetLensCorrection(bool enabled) {
  Settings()->setValue(Key(RawImportLookField::LensCorrection), enabled);
}

void RawImportLookSettings::Reset(RawImportLookField field) { Settings()->remove(Key(field)); }

void RawImportLookSettings::ResetAll() {
  const auto settings = Settings();
  for (const auto field : kAllFields) settings->remove(Key(field));
}

}  // namespace alcedo
