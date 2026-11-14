//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/raw_import_look_settings_model.hpp"

#include <QCoreApplication>
#include <QVariantMap>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/tone_curve_presets.hpp"
#include "ui/alcedo_main/editor_support/modules/curve.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {
namespace {

using alcedo::RawImportLookField;

constexpr auto kTranslationContext = "alcedo::ui::RawImportLookSettingsModel";

struct TranslatedName {
  std::string_view value;
  const char*      source;
};

// Display names of the Tone Curve presets by id: the names of the Tone Curve section menu.
constexpr std::array kToneCurvePresetNames = {
    TranslatedName{"linear", QT_TRANSLATE_NOOP("alcedo::ui::RawImportLookSettingsModel", "Linear")},
    TranslatedName{"soft_contrast",
                   QT_TRANSLATE_NOOP("alcedo::ui::RawImportLookSettingsModel", "Soft Contrast")},
    TranslatedName{"medium_contrast",
                   QT_TRANSLATE_NOOP("alcedo::ui::RawImportLookSettingsModel", "Medium Contrast")},
    TranslatedName{"strong_contrast",
                   QT_TRANSLATE_NOOP("alcedo::ui::RawImportLookSettingsModel", "Strong Contrast")},
    TranslatedName{"cl_digital",
                   QT_TRANSLATE_NOOP("alcedo::ui::RawImportLookSettingsModel", "Cl. Digit")},
    TranslatedName{"kodak_2383",
                   QT_TRANSLATE_NOOP("alcedo::ui::RawImportLookSettingsModel", "Kodak 2383")},
    TranslatedName{"fuji_velvia_50",
                   QT_TRANSLATE_NOOP("alcedo::ui::RawImportLookSettingsModel", "Velvia 50")},
    TranslatedName{"kodak_ektachrome_e100",
                   QT_TRANSLATE_NOOP("alcedo::ui::RawImportLookSettingsModel", "Ektachrome E100")},
};

// Menu groups of the Tone Curve presets (ToneCurvePreset::group).
constexpr std::array kToneCurveGroupNames = {
    TranslatedName{"Contrast",
                   QT_TRANSLATE_NOOP("alcedo::ui::RawImportLookSettingsModel", "Contrast")},
    TranslatedName{"Rendering",
                   QT_TRANSLATE_NOOP("alcedo::ui::RawImportLookSettingsModel", "Rendering")},
    TranslatedName{"Film", QT_TRANSLATE_NOOP("alcedo::ui::RawImportLookSettingsModel", "Film")},
};

constexpr std::array kDrtMethodNames = {
    TranslatedName{RawImportLookSettingsModel::kAces20MethodValue,
                   QT_TRANSLATE_NOOP("alcedo::ui::RawImportLookSettingsModel", "ACES 2.0")},
    TranslatedName{RawImportLookSettingsModel::kOpenDrtMethodValue,
                   QT_TRANSLATE_NOOP("alcedo::ui::RawImportLookSettingsModel", "OpenDRT")},
};

// Display names of the OpenDRT look presets: the names of the Display Transform Look menu.
constexpr std::array kOpenDrtLookNames = {
    TranslatedName{"standard",
                   QT_TRANSLATE_NOOP("alcedo::ui::RawImportLookSettingsModel", "Standard")},
    TranslatedName{"arriba", QT_TRANSLATE_NOOP("alcedo::ui::RawImportLookSettingsModel", "Arriba")},
    TranslatedName{"sylvan", QT_TRANSLATE_NOOP("alcedo::ui::RawImportLookSettingsModel", "Sylvan")},
    TranslatedName{"colorful",
                   QT_TRANSLATE_NOOP("alcedo::ui::RawImportLookSettingsModel", "Colorful")},
    TranslatedName{"aery", QT_TRANSLATE_NOOP("alcedo::ui::RawImportLookSettingsModel", "Aery")},
    TranslatedName{"dystopic",
                   QT_TRANSLATE_NOOP("alcedo::ui::RawImportLookSettingsModel", "Dystopic")},
    TranslatedName{"umbra", QT_TRANSLATE_NOOP("alcedo::ui::RawImportLookSettingsModel", "Umbra")},
};

constexpr RawImportLookField kAllFields[] = {
    RawImportLookField::ExposureEv,        RawImportLookField::Saturation,
    RawImportLookField::ToneCurvePreset,   RawImportLookField::DrtMethod,
    RawImportLookField::OpenDrtLookPreset, RawImportLookField::LensCorrection,
};

auto Text(std::string_view text) -> QString {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

/// The translated name of @p value in @p names; @p fallback when the table has no entry.
template <size_t N>
auto TranslatedNameOf(const std::array<TranslatedName, N>& names, std::string_view value,
                      std::string_view fallback) -> QString {
  const auto found = std::ranges::find(names, value, &TranslatedName::value);
  if (found == names.end()) return Text(fallback);
  return QCoreApplication::translate(kTranslationContext, found->source);
}

auto Entry(std::string_view value, QString label) -> QVariantMap {
  return QVariantMap{{QStringLiteral("value"), Text(value)},
                     {QStringLiteral("label"), std::move(label)}};
}

/// Exposure at the page step of 0.01 EV.
auto RoundExposure(double exposure_ev) -> double { return std::round(exposure_ev * 100.0) / 100.0; }

auto InClosedRange(double value, double min, double max) -> bool {
  return std::isfinite(value) && value >= min && value <= max;
}

auto MethodValue(DrtMethod method) -> std::string_view {
  return method == DrtMethod::Aces20 ? RawImportLookSettingsModel::kAces20MethodValue
                                     : RawImportLookSettingsModel::kOpenDrtMethodValue;
}

auto IsOpenDrtLookPreset(std::string_view preset) -> bool {
  const auto presets = alcedo::RawImportOpenDrtLookPresets();
  return std::ranges::find(presets, preset) != presets.end();
}

}  // namespace

RawImportLookSettingsModel::RawImportLookSettingsModel(QString settings_file, QObject* parent)
    : QObject(parent), settings_(std::move(settings_file)) {
  load();
  // The display names are translated; a language change changes them.
  QObject::connect(&i18n::TranslationNotifier::Instance(),
                   &i18n::TranslationNotifier::LanguageChanged, this,
                   &RawImportLookSettingsModel::labelsChanged);
}

auto RawImportLookSettingsModel::RowValuesOf(const alcedo::RawImportLook& look) -> RowValues {
  return RowValues{
      .exposure_ev          = RoundExposure(static_cast<double>(look.exposure_ev)),
      // The multiplier 1 + value / 100 back to the editor value at the page step of 1.
      .saturation           = std::round((static_cast<double>(look.saturation) - 1.0) * 100.0),
      .tone_curve_preset    = look.tone_curve_preset_id,
      .drt_method           = look.drt_method,
      .open_drt_look_preset = look.open_drt_look_preset,
      .lens_correction      = look.lens_correction,
  };
}

auto RawImportLookSettingsModel::AppDefaults() -> const RowValues& {
  static const RowValues defaults = RowValuesOf(alcedo::RawImportLook{});
  return defaults;
}

auto RawImportLookSettingsModel::FieldEqual(RawImportLookField field, const RowValues& lhs,
                                            const RowValues& rhs) -> bool {
  switch (field) {
    case RawImportLookField::ExposureEv:
      return lhs.exposure_ev == rhs.exposure_ev;
    case RawImportLookField::Saturation:
      return lhs.saturation == rhs.saturation;
    case RawImportLookField::ToneCurvePreset:
      return lhs.tone_curve_preset == rhs.tone_curve_preset;
    case RawImportLookField::DrtMethod:
      return lhs.drt_method == rhs.drt_method;
    case RawImportLookField::OpenDrtLookPreset:
      return lhs.open_drt_look_preset == rhs.open_drt_look_preset;
    case RawImportLookField::LensCorrection:
      return lhs.lens_correction == rhs.lens_correction;
  }
  throw std::invalid_argument("Unknown RAW import look field");
}

auto RawImportLookSettingsModel::tone_curve_preset() const -> QString {
  return Text(pending_.tone_curve_preset);
}

auto RawImportLookSettingsModel::drt_method() const -> QString {
  return Text(MethodValue(pending_.drt_method));
}

auto RawImportLookSettingsModel::open_drt_look_preset() const -> QString {
  return Text(pending_.open_drt_look_preset);
}

auto RawImportLookSettingsModel::IsModified(RawImportLookField field) const -> bool {
  return !FieldEqual(field, pending_, AppDefaults());
}

auto RawImportLookSettingsModel::exposure_ev_modified() const -> bool {
  return IsModified(RawImportLookField::ExposureEv);
}

auto RawImportLookSettingsModel::saturation_modified() const -> bool {
  return IsModified(RawImportLookField::Saturation);
}

auto RawImportLookSettingsModel::tone_curve_preset_modified() const -> bool {
  return IsModified(RawImportLookField::ToneCurvePreset);
}

auto RawImportLookSettingsModel::drt_method_modified() const -> bool {
  return IsModified(RawImportLookField::DrtMethod);
}

auto RawImportLookSettingsModel::open_drt_look_preset_modified() const -> bool {
  return IsModified(RawImportLookField::OpenDrtLookPreset);
}

auto RawImportLookSettingsModel::lens_correction_modified() const -> bool {
  return IsModified(RawImportLookField::LensCorrection);
}

auto RawImportLookSettingsModel::any_modified() const -> bool {
  return std::ranges::any_of(kAllFields, [this](auto field) { return IsModified(field); });
}

auto RawImportLookSettingsModel::exposure_ev_default() -> double {
  return AppDefaults().exposure_ev;
}

auto RawImportLookSettingsModel::saturation_default() -> double {
  return AppDefaults().saturation;
}

auto RawImportLookSettingsModel::tone_curve_presets() -> QVariantList {
  QVariantList list;
  for (const auto& preset : alcedo::ToneCurvePresets()) {
    auto entry =
        Entry(preset.id, TranslatedNameOf(kToneCurvePresetNames, preset.id, preset.display_name));
    entry.insert(QStringLiteral("group"),
                 preset.group.empty()
                     ? QString{}
                     : TranslatedNameOf(kToneCurveGroupNames, preset.group, preset.group));
    list.push_back(entry);
  }
  return list;
}

auto RawImportLookSettingsModel::drt_methods() -> QVariantList {
  QVariantList list;
  for (const auto& method : kDrtMethodNames) {
    list.push_back(
        Entry(method.value, QCoreApplication::translate(kTranslationContext, method.source)));
  }
  return list;
}

auto RawImportLookSettingsModel::open_drt_look_presets() -> QVariantList {
  QVariantList list;
  for (const auto preset : alcedo::RawImportOpenDrtLookPresets()) {
    list.push_back(Entry(preset, TranslatedNameOf(kOpenDrtLookNames, preset, preset)));
  }
  return list;
}

auto RawImportLookSettingsModel::axis_grey() -> double { return curve::kCurveAxisGrey; }

auto RawImportLookSettingsModel::axis_stop() -> double { return curve::kCurveAxisStop; }

void RawImportLookSettingsModel::load() {
  loaded_  = RowValuesOf(settings_.Read());
  pending_ = loaded_;
  RebuildCurve();
  SetErrorMessage({});
  emit pendingChanged();
}

auto RawImportLookSettingsModel::setExposureEv(double exposure_ev) -> bool {
  if (!InClosedRange(exposure_ev, kMinExposureEv, kMaxExposureEv)) {
    return Reject(tr("Exposure must be from -10 to +10 EV."));
  }
  pending_.exposure_ev = RoundExposure(exposure_ev);
  Accepted(false);
  return true;
}

auto RawImportLookSettingsModel::setSaturation(double saturation) -> bool {
  if (!InClosedRange(saturation, kMinSaturation, kMaxSaturation)) {
    return Reject(tr("Saturation must be from -100 to +100."));
  }
  pending_.saturation = std::round(saturation);
  Accepted(false);
  return true;
}

auto RawImportLookSettingsModel::setToneCurvePreset(const QString& preset_id) -> bool {
  const auto id = preset_id.toStdString();
  if (alcedo::FindToneCurvePreset(id) == nullptr) {
    return Reject(tr("No Tone Curve preset has the id %1.").arg(preset_id));
  }
  pending_.tone_curve_preset = id;
  Accepted(true);
  return true;
}

auto RawImportLookSettingsModel::setDrtMethod(const QString& method) -> bool {
  if (method == QLatin1String(kAces20MethodValue)) {
    pending_.drt_method = DrtMethod::Aces20;
  } else if (method == QLatin1String(kOpenDrtMethodValue)) {
    pending_.drt_method = DrtMethod::OpenDrt;
  } else {
    return Reject(tr("The display transform must be ACES 2.0 or OpenDRT."));
  }
  Accepted(false);
  return true;
}

auto RawImportLookSettingsModel::setOpenDrtLookPreset(const QString& preset) -> bool {
  const auto id = preset.toStdString();
  if (!IsOpenDrtLookPreset(id)) {
    return Reject(tr("No OpenDRT look preset has the id %1.").arg(preset));
  }
  pending_.open_drt_look_preset = id;
  Accepted(false);
  return true;
}

void RawImportLookSettingsModel::setLensCorrection(bool enabled) {
  pending_.lens_correction = enabled;
  Accepted(false);
}

auto RawImportLookSettingsModel::resetRow(const QString& row) -> bool {
  static const std::array<std::pair<QLatin1String, RawImportLookField>, 6> kRows = {{
      {QLatin1String("exposureEv"), RawImportLookField::ExposureEv},
      {QLatin1String("saturation"), RawImportLookField::Saturation},
      {QLatin1String("toneCurvePreset"), RawImportLookField::ToneCurvePreset},
      {QLatin1String("drtMethod"), RawImportLookField::DrtMethod},
      {QLatin1String("openDrtLookPreset"), RawImportLookField::OpenDrtLookPreset},
      {QLatin1String("lensCorrection"), RawImportLookField::LensCorrection},
  }};
  for (const auto& [name, field] : kRows) {
    if (row == name) {
      ResetField(field);
      return true;
    }
  }
  return false;
}

void RawImportLookSettingsModel::ResetField(RawImportLookField field) {
  const auto& defaults = AppDefaults();
  switch (field) {
    case RawImportLookField::ExposureEv:
      pending_.exposure_ev = defaults.exposure_ev;
      break;
    case RawImportLookField::Saturation:
      pending_.saturation = defaults.saturation;
      break;
    case RawImportLookField::ToneCurvePreset:
      pending_.tone_curve_preset = defaults.tone_curve_preset;
      break;
    case RawImportLookField::DrtMethod:
      pending_.drt_method = defaults.drt_method;
      break;
    case RawImportLookField::OpenDrtLookPreset:
      pending_.open_drt_look_preset = defaults.open_drt_look_preset;
      break;
    case RawImportLookField::LensCorrection:
      pending_.lens_correction = defaults.lens_correction;
      break;
  }
  Accepted(field == RawImportLookField::ToneCurvePreset);
}

void RawImportLookSettingsModel::resetAll() {
  pending_ = AppDefaults();
  Accepted(true);
}

void RawImportLookSettingsModel::WriteField(RawImportLookField field) {
  if (!IsModified(field)) {
    settings_.Reset(field);
    return;
  }
  switch (field) {
    case RawImportLookField::ExposureEv:
      settings_.SetExposureEv(pending_.exposure_ev);
      return;
    case RawImportLookField::Saturation:
      settings_.SetSaturationPercent(pending_.saturation);
      return;
    case RawImportLookField::ToneCurvePreset:
      settings_.SetToneCurvePreset(pending_.tone_curve_preset);
      return;
    case RawImportLookField::DrtMethod:
      settings_.SetDrtMethod(pending_.drt_method);
      return;
    case RawImportLookField::OpenDrtLookPreset:
      settings_.SetOpenDrtLookPreset(pending_.open_drt_look_preset);
      return;
    case RawImportLookField::LensCorrection:
      settings_.SetLensCorrection(pending_.lens_correction);
      return;
  }
}

auto RawImportLookSettingsModel::apply() -> bool {
  try {
    for (const auto field : kAllFields) {
      if (!FieldEqual(field, pending_, loaded_)) WriteField(field);
    }
  } catch (const std::invalid_argument& error) {
    SetErrorMessage(QString::fromUtf8(error.what()));
    return false;
  }
  load();
  return true;
}

void RawImportLookSettingsModel::RebuildCurve() {
  const auto&          preset = alcedo::ResolveToneCurvePreset(pending_.tone_curve_preset);
  std::vector<QPointF> points;
  points.reserve(preset.points.size());
  for (const auto& point : preset.points) points.emplace_back(point.x, point.y);
  points        = curve::NormalizeCurveControlPoints(points);
  curve_points_ = QList<QPointF>(points.begin(), points.end());
  curve_line_   = curve::SampleCurveLine(curve::SolveCurveSpline(points));
}

void RawImportLookSettingsModel::SetErrorMessage(QString message) {
  if (error_message_ == message) return;
  error_message_ = std::move(message);
  emit errorMessageChanged();
}

auto RawImportLookSettingsModel::Reject(QString message) -> bool {
  SetErrorMessage(std::move(message));
  // The control of the row shows the kept value again.
  emit pendingChanged();
  return false;
}

void RawImportLookSettingsModel::Accepted(bool curve_changed) {
  if (curve_changed) RebuildCurve();
  SetErrorMessage({});
  emit pendingChanged();
}

}  // namespace alcedo::ui
