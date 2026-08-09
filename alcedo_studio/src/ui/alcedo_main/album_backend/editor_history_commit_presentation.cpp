//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_commit_presentation.hpp"

#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include "app/editor_adjustment_pipeline.hpp"
#include "edit/operators/op_base.hpp"

namespace alcedo::ui {
namespace {

// ---- Formatting primitives (ported from legacy history_cards.cpp so the QML
// timeline reuses the same value rendering without parsing commit JSON). ----

auto FormatNumber(double v) -> QString {
  if (std::fabs(v - std::round(v)) < 1e-4 && std::fabs(v) < 1e9) {
    return QString::number(static_cast<long long>(std::llround(v)));
  }
  QString s = QString::number(v, 'f', 2);
  while (s.endsWith(QLatin1Char('0'))) {
    s.chop(1);
  }
  if (s.endsWith(QLatin1Char('.'))) {
    s.chop(1);
  }
  return s;
}

auto FormatSigned(double v) -> QString {
  const QString body = FormatNumber(std::fabs(v));
  if (v > 0.0) {
    return QStringLiteral("+") + body;
  }
  if (v < 0.0) {
    return QStringLiteral("-") + body;
  }
  return body;
}

auto WithUnit(const QString& value, const QString& unit, bool space_before_unit = false)
    -> QString {
  if (value.isEmpty() || unit.isEmpty()) {
    return value;
  }
  return space_before_unit ? value + QStringLiteral(" ") + unit : value + unit;
}

auto JsonAtPath(const nlohmann::json& root, std::initializer_list<const char*> path)
    -> const nlohmann::json* {
  const nlohmann::json* node = &root;
  for (const char* key : path) {
    if (!node->is_object()) {
      return nullptr;
    }
    const auto it = node->find(key);
    if (it == node->end()) {
      return nullptr;
    }
    node = &(*it);
  }
  return node;
}

auto JsonNumberAtPath(const nlohmann::json& root, std::initializer_list<const char*> path)
    -> std::optional<double> {
  const auto* node = JsonAtPath(root, path);
  if (!node) {
    return std::nullopt;
  }
  if (node->is_number_float()) {
    return node->get<double>();
  }
  if (node->is_number_integer()) {
    return static_cast<double>(node->get<long long>());
  }
  if (node->is_number_unsigned()) {
    return static_cast<double>(node->get<unsigned long long>());
  }
  return std::nullopt;
}

auto JsonBoolAtPath(const nlohmann::json& root, std::initializer_list<const char*> path)
    -> std::optional<bool> {
  const auto* node = JsonAtPath(root, path);
  if (!node || !node->is_boolean()) {
    return std::nullopt;
  }
  return node->get<bool>();
}

auto JsonStringAtPath(const nlohmann::json& root, std::initializer_list<const char*> path)
    -> std::optional<QString> {
  const auto* node = JsonAtPath(root, path);
  if (!node || !node->is_string()) {
    return std::nullopt;
  }
  return QString::fromStdString(node->get<std::string>());
}

auto JsonArrayNumber(const nlohmann::json* array, std::size_t index) -> std::optional<double> {
  if (!array || !array->is_array() || index >= array->size()) {
    return std::nullopt;
  }
  const auto& node = (*array)[index];
  if (node.is_number_float()) {
    return node.get<double>();
  }
  if (node.is_number_integer()) {
    return static_cast<double>(node.get<long long>());
  }
  if (node.is_number_unsigned()) {
    return static_cast<double>(node.get<unsigned long long>());
  }
  return std::nullopt;
}

auto PrettyToken(QString raw) -> QString {
  if (raw.isEmpty()) {
    return raw;
  }
  static const std::pair<const char*, const char*> kMap[] = {
      {"as_shot", "As Shot"},
      {"custom", "Custom"},
      {"REC709", "Rec.709"},
      {"DISPLAY_P3", "Display P3"},
      {"SRGB", "sRGB"},
      {"ACESCG", "ACEScg"},
      {"GAMMA_2_2", "Gamma 2.2"},
      {"GAMMA_2_4", "Gamma 2.4"},
      {"ST2084", "PQ"},
      {"OPEN_DRT", "OpenDRT"},
      {"HLG", "HLG"},
  };
  for (const auto& [from, to] : kMap) {
    if (raw.compare(QLatin1String(from), Qt::CaseInsensitive) == 0) {
      return QString::fromLatin1(to);
    }
  }
  raw.replace(QLatin1Char('_'), QLatin1Char(' '));
  raw.replace(QLatin1Char('-'), QLatin1Char(' '));
  const QStringList parts = raw.split(QLatin1Char(' '), Qt::SkipEmptyParts);
  QStringList       normalized;
  normalized.reserve(parts.size());
  for (const QString& part : parts) {
    const QString lower = part.toLower();
    if (part == part.toUpper() && part.size() <= 4) {
      normalized.push_back(part);
    } else {
      normalized.push_back(lower.left(1).toUpper() + lower.mid(1));
    }
  }
  return normalized.join(QLatin1Char(' '));
}

// Per-operator display name and glyph used by the QML history rail.

auto DisplayName(OperatorType op) -> QString {
  switch (op) {
    case OperatorType::RAW_DECODE:
      return QStringLiteral("RAW Decode");
    case OperatorType::RESIZE:
      return QStringLiteral("Resize");
    case OperatorType::CROP_ROTATE:
      return QStringLiteral("Crop / Rotate");
    case OperatorType::EXPOSURE:
      return QStringLiteral("Exposure");
    case OperatorType::CONTRAST:
      return QStringLiteral("Contrast");
    case OperatorType::WHITE:
      return QStringLiteral("Whites");
    case OperatorType::BLACK:
      return QStringLiteral("Blacks");
    case OperatorType::SHADOWS:
      return QStringLiteral("Shadows");
    case OperatorType::HIGHLIGHTS:
      return QStringLiteral("Highlights");
    case OperatorType::CURVE:
      return QStringLiteral("Curve");
    case OperatorType::HLS:
      return QStringLiteral("HSL");
    case OperatorType::SATURATION:
      return QStringLiteral("Saturation");
    case OperatorType::TINT:
      return QStringLiteral("Tint");
    case OperatorType::VIBRANCE:
      return QStringLiteral("Vibrance");
    case OperatorType::CST:
      return QStringLiteral("Color Space");
    case OperatorType::TO_WS:
      return QStringLiteral("To Working Space");
    case OperatorType::TO_OUTPUT:
      return QStringLiteral("To Output");
    case OperatorType::LMT:
      return QStringLiteral("LUT");
    case OperatorType::ODT:
      return QStringLiteral("ODT");
    case OperatorType::CLARITY:
      return QStringLiteral("Clarity");
    case OperatorType::SHARPEN:
      return QStringLiteral("Sharpen");
    case OperatorType::COLOR_WHEEL:
      return QStringLiteral("Color Wheel");
    case OperatorType::ACES_TONE_MAPPING:
      return QStringLiteral("ACES Tone");
    case OperatorType::AUTO_EXPOSURE:
      return QStringLiteral("Auto Exposure");
    case OperatorType::LENS_CALIBRATION:
      return QStringLiteral("Lens Profile");
    case OperatorType::COLOR_TEMP:
      return QStringLiteral("Color Temp");
    case OperatorType::FILM_GRAIN:
      return QStringLiteral("Grain");
    case OperatorType::HALATION:
      return QStringLiteral("Halation");
    case OperatorType::UNKNOWN:
      return QStringLiteral("Edit");
  }
  return QStringLiteral("Edit");
}

auto IconResource(OperatorType op) -> QString {
  switch (op) {
    case OperatorType::RAW_DECODE:
      return QStringLiteral(":/history_icons/scan-search.svg");
    case OperatorType::RESIZE:
      return QStringLiteral(":/history_icons/scaling.svg");
    case OperatorType::CROP_ROTATE:
      return QStringLiteral(":/history_icons/crop.svg");
    case OperatorType::EXPOSURE:
      return QStringLiteral(":/history_icons/sun-medium.svg");
    case OperatorType::CONTRAST:
      return QStringLiteral(":/history_icons/contrast.svg");
    case OperatorType::WHITE:
      return QStringLiteral(":/history_icons/sun.svg");
    case OperatorType::BLACK:
      return QStringLiteral(":/history_icons/moon.svg");
    case OperatorType::SHADOWS:
      return QStringLiteral(":/history_icons/square-split-horizontal.svg");
    case OperatorType::HIGHLIGHTS:
      return QStringLiteral(":/history_icons/sparkles.svg");
    case OperatorType::CURVE:
      return QStringLiteral(":/history_icons/chart-spline.svg");
    case OperatorType::HLS:
      return QStringLiteral(":/history_icons/swatch-book.svg");
    case OperatorType::SATURATION:
      return QStringLiteral(":/history_icons/droplets.svg");
    case OperatorType::TINT:
      return QStringLiteral(":/history_icons/pipette.svg");
    case OperatorType::VIBRANCE:
      return QStringLiteral(":/history_icons/sparkles.svg");
    case OperatorType::CST:
      return QStringLiteral(":/history_icons/arrow-right-left.svg");
    case OperatorType::TO_WS:
      return QStringLiteral(":/history_icons/workflow.svg");
    case OperatorType::TO_OUTPUT:
      return QStringLiteral(":/history_icons/monitor-up.svg");
    case OperatorType::LMT:
      return QStringLiteral(":/history_icons/file-sliders.svg");
    case OperatorType::ODT:
      return QStringLiteral(":/history_icons/monitor.svg");
    case OperatorType::CLARITY:
      return QStringLiteral(":/history_icons/focus.svg");
    case OperatorType::SHARPEN:
    case OperatorType::FILM_GRAIN:
      return QStringLiteral(":/history_icons/scan-line.svg");
    case OperatorType::COLOR_WHEEL:
      return QStringLiteral(":/history_icons/palette.svg");
    case OperatorType::ACES_TONE_MAPPING:
      return QStringLiteral(":/history_icons/git-commit-horizontal.svg");
    case OperatorType::AUTO_EXPOSURE:
      return QStringLiteral(":/history_icons/wand-sparkles.svg");
    case OperatorType::LENS_CALIBRATION:
      return QStringLiteral(":/history_icons/aperture.svg");
    case OperatorType::COLOR_TEMP:
      return QStringLiteral(":/history_icons/thermometer.svg");
    case OperatorType::HALATION:
      return QStringLiteral(":/history_icons/sun.svg");
    case OperatorType::UNKNOWN:
      return QStringLiteral(":/history_icons/sliders-horizontal.svg");
  }
  return QStringLiteral(":/history_icons/sliders-horizontal.svg");
}

// ---- Per-operator value formatting. Each helper receives the after (new) and
// before (old) operator params and returns the split before/after text plus a
// contextual delta line. ----

struct CommitSummary {
  QString before_text;
  QString after_text;
  QString delta_text;
};

auto SignedScalar(const std::optional<double>& old_v, const std::optional<double>& new_v,
                  const QString& unit = QString(), bool space_before_unit = false)
    -> CommitSummary {
  if (!new_v.has_value()) {
    return {};
  }
  const QString after = WithUnit(FormatSigned(*new_v), unit, space_before_unit);
  const QString before =
      old_v.has_value() ? WithUnit(FormatSigned(*old_v), unit, space_before_unit) : QString();
  const QString delta = before.isEmpty() ? QStringLiteral("Set to %1").arg(after)
                                         : QStringLiteral("%1 \u2192 %2").arg(before, after);
  return {before, after, delta};
}

auto LensName(const nlohmann::json& params) -> QString {
  const QString maker = JsonStringAtPath(params, {"lens_calib", "lens_maker"}).value_or(QString());
  const QString model = JsonStringAtPath(params, {"lens_calib", "lens_model"}).value_or(QString());
  if (!maker.isEmpty() && !model.isEmpty()) {
    return maker + QStringLiteral(" ") + model;
  }
  if (!model.isEmpty()) {
    return model;
  }
  if (!maker.isEmpty()) {
    return maker;
  }
  return {};
}

auto ColorTempLabel(const nlohmann::json& params) -> QString {
  const QString mode =
      PrettyToken(JsonStringAtPath(params, {"color_temp", "mode"}).value_or(QString()));
  if (mode.compare(QStringLiteral("Custom"), Qt::CaseInsensitive) == 0) {
    const auto cct  = JsonNumberAtPath(params, {"color_temp", "cct"});
    const auto tint = JsonNumberAtPath(params, {"color_temp", "tint"});
    if (cct.has_value()) {
      QString label = WithUnit(FormatNumber(*cct), QStringLiteral("K"));
      if (tint.has_value() && std::fabs(*tint) > 1e-4) {
        label += QStringLiteral(" / ") + FormatSigned(*tint);
      }
      return label;
    }
  }
  return mode.isEmpty() ? QStringLiteral("Color Temp") : mode;
}

auto OdtTargetLabel(const nlohmann::json& params) -> QString {
  const QString space =
      PrettyToken(JsonStringAtPath(params, {"odt", "encoding_space"}).value_or(QString()));
  const QString eotf =
      PrettyToken(JsonStringAtPath(params, {"odt", "encoding_eotf"}).value_or(QString()));
  if (!space.isEmpty() && !eotf.isEmpty()) {
    return space + QStringLiteral(" / ") + eotf;
  }
  if (!space.isEmpty()) {
    return space;
  }
  if (!eotf.isEmpty()) {
    return eotf;
  }
  return QStringLiteral("Output");
}

auto CropAreaPercent(const nlohmann::json& params) -> std::optional<double> {
  const auto w = JsonNumberAtPath(params, {"crop_rotate", "crop_rect", "w"});
  const auto h = JsonNumberAtPath(params, {"crop_rotate", "crop_rect", "h"});
  if (!w.has_value() || !h.has_value()) {
    return std::nullopt;
  }
  return std::clamp(*w * *h * 100.0, 0.0, 100.0);
}

auto FormatCropAspect(const nlohmann::json& params) -> QString {
  const QString preset =
      JsonStringAtPath(params, {"crop_rotate", "aspect_ratio_preset"}).value_or(QString());
  if (!preset.isEmpty() && preset.compare(QStringLiteral("free"), Qt::CaseInsensitive) != 0) {
    return PrettyToken(preset);
  }
  const auto w = JsonNumberAtPath(params, {"crop_rotate", "aspect_ratio", "width"});
  const auto h = JsonNumberAtPath(params, {"crop_rotate", "aspect_ratio", "height"});
  if (!w.has_value() || !h.has_value() || *w <= 0.0 || *h <= 0.0) {
    return QStringLiteral("Free");
  }
  return QStringLiteral("%1:%2").arg(FormatNumber(*w), FormatNumber(*h));
}

auto CurvePointCount(const nlohmann::json& params) -> std::optional<std::size_t> {
  const auto* points = JsonAtPath(params, {"curve", "points"});
  if (!points || !points->is_array()) {
    return std::nullopt;
  }
  return points->size();
}

auto SummarizeRawDecode(const nlohmann::json& after, const nlohmann::json& before)
    -> CommitSummary {
  const auto next = JsonBoolAtPath(after, {"raw", "highlights_reconstruct"});
  const auto old  = JsonBoolAtPath(before, {"raw", "highlights_reconstruct"});
  if (!next.has_value()) {
    return {};
  }
  const QString after_text = next.value() ? QStringLiteral("Recover") : QStringLiteral("Basic");
  const QString before_text =
      old.has_value() ? (old.value() ? QStringLiteral("Recover") : QStringLiteral("Basic"))
                      : QString();
  const QString delta = old.has_value() && *old != *next
                            ? QStringLiteral("Highlights %1 \u2192 %2")
                                  .arg(old.value() ? QStringLiteral("on") : QStringLiteral("off"),
                                       next.value() ? QStringLiteral("on") : QStringLiteral("off"))
                            : after_text;
  return {before_text, after_text, delta};
}

auto SummarizeLens(const nlohmann::json& after, const nlohmann::json& before) -> CommitSummary {
  const bool    enabled  = JsonBoolAtPath(after, {"lens_calib", "enabled"}).value_or(true);
  const auto    old_en   = JsonBoolAtPath(before, {"lens_calib", "enabled"});
  const QString lens     = LensName(after);
  const QString old_lens = LensName(before);
  if (!old_lens.isEmpty() && lens != old_lens) {
    return {old_lens, lens.isEmpty() ? QStringLiteral("Auto") : lens,
            QStringLiteral("%1 \u2192 %2")
                .arg(old_lens, lens.isEmpty() ? QStringLiteral("Auto") : lens)};
  }
  if (!lens.isEmpty()) {
    return {old_lens.isEmpty() ? QString() : old_lens, lens, lens};
  }
  if (old_en.has_value() && *old_en != enabled) {
    const QString o = old_en.value() ? QStringLiteral("on") : QStringLiteral("off");
    const QString n = enabled ? QStringLiteral("on") : QStringLiteral("off");
    return {o, n, QStringLiteral("Profile %1 \u2192 %2").arg(o, n)};
  }
  const QString label = enabled ? QStringLiteral("Auto lens profile") : QStringLiteral("Off");
  return {QString(), label, label};
}

auto SummarizeColorTemp(const nlohmann::json& after, const nlohmann::json& before)
    -> CommitSummary {
  const QString value     = ColorTempLabel(after);
  const QString old_value = ColorTempLabel(before);
  if (!old_value.isEmpty() && old_value != value) {
    return {old_value, value, QStringLiteral("%1 \u2192 %2").arg(old_value, value)};
  }
  return {old_value.isEmpty() ? QString() : old_value, value,
          value.isEmpty() ? QStringLiteral("White balance updated") : value};
}

auto SummarizeHls(const nlohmann::json& after, const nlohmann::json& before) -> CommitSummary {
  constexpr std::array<const char*, 8> kHueLabels       = {"Red",  "Orange", "Yellow", "Green",
                                                           "Cyan", "Blue",   "Purple", "Magenta"};
  constexpr std::array<const char*, 3> kComponentLabels = {"Hue", "Light", "Chroma"};

  const auto*                          table       = JsonAtPath(after, {"HLS", "hls_adj_table"});
  const auto*                          old_table   = JsonAtPath(before, {"HLS", "hls_adj_table"});
  const auto*                          range_table = JsonAtPath(after, {"HLS", "h_range_table"});
  const auto*                          old_range   = JsonAtPath(before, {"HLS", "h_range_table"});

  if (table && table->is_array()) {
    for (std::size_t i = 0; i < table->size() && i < kHueLabels.size(); ++i) {
      const auto&           row     = (*table)[i];
      const nlohmann::json* old_row = (old_table && old_table->is_array() && i < old_table->size())
                                          ? &(*old_table)[i]
                                          : nullptr;
      if (!row.is_array()) {
        continue;
      }
      for (std::size_t j = 0; j < 3 && j < row.size(); ++j) {
        const auto next = JsonArrayNumber(&row, j);
        const auto old  = old_row ? JsonArrayNumber(old_row, j) : std::nullopt;
        if (!next.has_value() || (old.has_value() && std::fabs(*old - *next) <= 1e-6)) {
          continue;
        }
        const bool    is_hue     = j == 0;
        const double  scale      = is_hue ? 1.0 : 1000.0;
        const QString unit       = is_hue ? QStringLiteral("\u00b0") : QString();
        const QString after_text = WithUnit(FormatSigned(*next * scale), unit);
        const QString before_text =
            old.has_value() ? WithUnit(FormatSigned(*old * scale), unit) : QString();
        const QString prefix = QStringLiteral("%1 %2").arg(
            QString::fromLatin1(kHueLabels[i]), QString::fromLatin1(kComponentLabels[j]));
        const QString delta =
            before_text.isEmpty()
                ? QStringLiteral("%1 %2").arg(prefix, after_text)
                : QStringLiteral("%1 %2 \u2192 %3").arg(prefix, before_text, after_text);
        return {before_text.isEmpty() ? QString() : (prefix + QStringLiteral(" ") + before_text),
                prefix + QStringLiteral(" ") + after_text, delta};
      }
    }
  }

  if (range_table && range_table->is_array()) {
    for (std::size_t i = 0; i < range_table->size() && i < kHueLabels.size(); ++i) {
      const auto next = JsonArrayNumber(range_table, i);
      const auto old  = old_range ? JsonArrayNumber(old_range, i) : std::nullopt;
      if (!next.has_value() || (old.has_value() && std::fabs(*old - *next) <= 1e-6)) {
        continue;
      }
      const QString after_text = WithUnit(FormatNumber(*next), QStringLiteral("\u00b0"));
      const QString before_text =
          old.has_value() ? WithUnit(FormatNumber(*old), QStringLiteral("\u00b0")) : QString();
      const QString hue = QString::fromLatin1(kHueLabels[i]);
      const QString delta =
          before_text.isEmpty()
              ? QStringLiteral("%1 Smoothness %2").arg(hue, after_text)
              : QStringLiteral("%1 Smoothness %2 \u2192 %3").arg(hue, before_text, after_text);
      return {before_text.isEmpty() ? QString()
                                    : QStringLiteral("%1 Smoothness %2").arg(hue, before_text),
              QStringLiteral("%1 Smoothness %2").arg(hue, after_text), delta};
    }
  }
  return {};
}

auto SummarizeColorWheel(const nlohmann::json& after, const nlohmann::json& before)
    -> CommitSummary {
  static const std::array<std::pair<const char*, const char*>, 3> kWheels = {
      {{"lift", "Lift"}, {"gamma", "Gamma"}, {"gain", "Gain"}}};
  for (const auto& [key, label] : kWheels) {
    const auto strength     = JsonNumberAtPath(after, {"color_wheel", key, "strength"});
    const auto old_strength = JsonNumberAtPath(before, {"color_wheel", key, "strength"});
    if (strength.has_value() &&
        (!old_strength.has_value() || std::fabs(*strength - *old_strength) > 1e-6)) {
      const QString after_text = FormatNumber(*strength);
      const QString before_text =
          old_strength.has_value() ? FormatNumber(*old_strength) : QString();
      return {before_text.isEmpty()
                  ? QString()
                  : QStringLiteral("%1 Strength %2").arg(QString::fromLatin1(label), before_text),
              QStringLiteral("%1 Strength %2").arg(QString::fromLatin1(label), after_text),
              QStringLiteral("%1 Strength %2 \u2192 %3")
                  .arg(QString::fromLatin1(label), before_text, after_text)};
    }
    const auto lum     = JsonNumberAtPath(after, {"color_wheel", key, "luminance_offset"});
    const auto old_lum = JsonNumberAtPath(before, {"color_wheel", key, "luminance_offset"});
    if (lum.has_value() && (!old_lum.has_value() || std::fabs(*lum - *old_lum) > 1e-6)) {
      const QString after_text  = FormatSigned(*lum);
      const QString before_text = old_lum.has_value() ? FormatSigned(*old_lum) : QString();
      return {before_text.isEmpty()
                  ? QString()
                  : QStringLiteral("%1 Lum %2").arg(QString::fromLatin1(label), before_text),
              QStringLiteral("%1 Lum %2").arg(QString::fromLatin1(label), after_text),
              QStringLiteral("%1 Lum %2 \u2192 %3")
                  .arg(QString::fromLatin1(label), before_text, after_text)};
    }
  }
  return {};
}

auto SummarizeCurve(const nlohmann::json& after, const nlohmann::json& before) -> CommitSummary {
  const auto next_pts = CurvePointCount(after);
  const auto old_pts  = CurvePointCount(before);
  if (!next_pts.has_value()) {
    return {};
  }
  const QString after_text = QStringLiteral("%1 pts").arg(static_cast<int>(*next_pts));
  const QString before_text =
      old_pts.has_value() ? QStringLiteral("%1 pts").arg(static_cast<int>(*old_pts)) : QString();
  const QString delta =
      before_text.isEmpty()
          ? QStringLiteral("Tone curve updated")
          : QStringLiteral("%1 \u2192 %2 control points").arg(before_text, after_text);
  return {before_text, after_text, delta};
}

auto SummarizeLut(const nlohmann::json& after, const nlohmann::json& before) -> CommitSummary {
  const QString path      = JsonStringAtPath(after, {"ocio_lmt"}).value_or(QString());
  const QString old_path  = JsonStringAtPath(before, {"ocio_lmt"}).value_or(QString());
  const QString file_name = path.isEmpty() ? QString() : QFileInfo(path).fileName();
  const QString old_file  = old_path.isEmpty() ? QString() : QFileInfo(old_path).fileName();
  if (path.isEmpty()) {
    const QString before_text = old_file.isEmpty() ? QStringLiteral("None") : old_file;
    return {before_text, QStringLiteral("Cleared"),
            QStringLiteral("%1 \u2192 Cleared").arg(before_text)};
  }
  const QString after_text = file_name.isEmpty() ? QStringLiteral("Loaded") : file_name;
  const QString before_text =
      old_path.isEmpty() ? QString() : (old_file.isEmpty() ? QStringLiteral("None") : old_file);
  return {before_text, after_text,
          before_text.isEmpty() ? after_text
                                : QStringLiteral("%1 \u2192 %2").arg(before_text, after_text)};
}

auto SummarizeOdt(const nlohmann::json& after, const nlohmann::json& before) -> CommitSummary {
  const auto    peak       = JsonNumberAtPath(after, {"odt", "peak_luminance"});
  const auto    old_peak   = JsonNumberAtPath(before, {"odt", "peak_luminance"});
  const QString target     = OdtTargetLabel(after);
  const QString old_target = OdtTargetLabel(before);
  if (peak.has_value() && (!old_peak.has_value() || std::fabs(*peak - *old_peak) > 1e-6)) {
    const QString after_text  = WithUnit(FormatNumber(*peak), QStringLiteral("nit"), true);
    const QString before_text = old_peak.has_value()
                                    ? WithUnit(FormatNumber(*old_peak), QStringLiteral("nit"), true)
                                    : QString();
    return {before_text, after_text,
            before_text.isEmpty() ? target
                                  : QStringLiteral("%1 \u2192 %2").arg(before_text, after_text)};
  }
  if (!target.isEmpty() && target != old_target && !old_target.isEmpty()) {
    return {old_target, target, QStringLiteral("%1 \u2192 %2").arg(old_target, target)};
  }
  return {old_target.isEmpty() ? QString() : old_target,
          target.isEmpty() ? QStringLiteral("Output") : target,
          QStringLiteral("Output transform updated")};
}

auto SummarizeCropRotate(const nlohmann::json& after, const nlohmann::json& before)
    -> CommitSummary {
  const auto angle     = JsonNumberAtPath(after, {"crop_rotate", "angle_degrees"});
  const auto old_angle = JsonNumberAtPath(before, {"crop_rotate", "angle_degrees"});
  if (angle.has_value() && (!old_angle.has_value() || std::fabs(*angle - *old_angle) > 1e-6)) {
    return SignedScalar(old_angle, angle, QStringLiteral("\u00b0"));
  }
  const bool crop_enabled = JsonBoolAtPath(after, {"crop_rotate", "enable_crop"}).value_or(false);
  const bool old_crop     = JsonBoolAtPath(before, {"crop_rotate", "enable_crop"}).value_or(false);
  const auto area         = CropAreaPercent(after);
  const auto old_area     = CropAreaPercent(before);
  if ((crop_enabled || old_crop) && area.has_value() &&
      (!old_area.has_value() || std::fabs(*area - *old_area) > 1e-4 || crop_enabled != old_crop)) {
    const QString after_text = QStringLiteral("%1%").arg(FormatNumber(*area));
    const QString before_text =
        old_area.has_value() ? QStringLiteral("%1%").arg(FormatNumber(*old_area)) : QString();
    return {before_text, after_text,
            before_text.isEmpty() ? QStringLiteral("Crop area %1").arg(after_text)
                                  : QStringLiteral("%1 \u2192 %2").arg(before_text, after_text)};
  }
  const QString aspect     = FormatCropAspect(after);
  const QString old_aspect = FormatCropAspect(before);
  if (!aspect.isEmpty() && aspect != old_aspect && !old_aspect.isEmpty()) {
    return {old_aspect, aspect, QStringLiteral("%1 \u2192 %2").arg(old_aspect, aspect)};
  }
  return {QString(), QStringLiteral("Crop"), QStringLiteral("Geometry updated")};
}

auto BuildSummary(OperatorType op, const nlohmann::json& after, const nlohmann::json& before)
    -> CommitSummary {
  switch (op) {
    case OperatorType::EXPOSURE:
      return SignedScalar(JsonNumberAtPath(before, {"exposure"}),
                          JsonNumberAtPath(after, {"exposure"}));
    case OperatorType::CONTRAST:
      return SignedScalar(JsonNumberAtPath(before, {"contrast"}),
                          JsonNumberAtPath(after, {"contrast"}));
    case OperatorType::WHITE:
      return SignedScalar(JsonNumberAtPath(before, {"white"}), JsonNumberAtPath(after, {"white"}));
    case OperatorType::BLACK:
      return SignedScalar(JsonNumberAtPath(before, {"black"}), JsonNumberAtPath(after, {"black"}));
    case OperatorType::SHADOWS:
      return SignedScalar(JsonNumberAtPath(before, {"shadows"}),
                          JsonNumberAtPath(after, {"shadows"}));
    case OperatorType::HIGHLIGHTS:
      return SignedScalar(JsonNumberAtPath(before, {"highlights"}),
                          JsonNumberAtPath(after, {"highlights"}));
    case OperatorType::SATURATION:
      return SignedScalar(JsonNumberAtPath(before, {"saturation"}),
                          JsonNumberAtPath(after, {"saturation"}));
    case OperatorType::VIBRANCE:
      return SignedScalar(JsonNumberAtPath(before, {"vibrance"}),
                          JsonNumberAtPath(after, {"vibrance"}));
    case OperatorType::CLARITY:
      return SignedScalar(JsonNumberAtPath(before, {"clarity"}),
                          JsonNumberAtPath(after, {"clarity"}));
    case OperatorType::SHARPEN:
      return SignedScalar(JsonNumberAtPath(before, {"sharpen", "offset"}),
                          JsonNumberAtPath(after, {"sharpen", "offset"}));
    case OperatorType::FILM_GRAIN:
      return SignedScalar(JsonNumberAtPath(before, {"film_grain", "strength"}),
                          JsonNumberAtPath(after, {"film_grain", "strength"}));
    case OperatorType::HALATION:
      return SignedScalar(JsonNumberAtPath(before, {"halation", "strength"}),
                          JsonNumberAtPath(after, {"halation", "strength"}));
    case OperatorType::RAW_DECODE:
      return SummarizeRawDecode(after, before);
    case OperatorType::LENS_CALIBRATION:
      return SummarizeLens(after, before);
    case OperatorType::COLOR_TEMP:
      return SummarizeColorTemp(after, before);
    case OperatorType::HLS:
      return SummarizeHls(after, before);
    case OperatorType::COLOR_WHEEL:
      return SummarizeColorWheel(after, before);
    case OperatorType::CURVE:
      return SummarizeCurve(after, before);
    case OperatorType::LMT:
      return SummarizeLut(after, before);
    case OperatorType::ODT:
      return SummarizeOdt(after, before);
    case OperatorType::CROP_ROTATE:
      return SummarizeCropRotate(after, before);
    default:
      break;
  }
  return {};
}

}  // namespace

auto PresentEditorHistoryCommit(const std::string& field_key, const std::string& before_value_json,
                                const std::string& after_value_json, bool before_enabled,
                                bool after_enabled, EditCommitKind kind,
                                const std::vector<std::string>& merge_field_keys)
    -> EditorHistoryCommitPresentation {
  EditorHistoryCommitPresentation out;
  if (kind == EditCommitKind::kMerge || field_key == "merge") {
    out.is_merge     = true;
    out.display_name = QStringLiteral("Merge");
    out.icon_key     = QStringLiteral(":/history_icons/git-commit-horizontal.svg");
    if (!merge_field_keys.empty()) {
      out.merge_summary = QStringLiteral("Resolved %1 field%2")
                              .arg(static_cast<int>(merge_field_keys.size()))
                              .arg(merge_field_keys.size() == 1 ? QString() : QStringLiteral("s"));
    } else {
      out.merge_summary = QStringLiteral("Resolved incoming adjustments");
    }
    return out;
  }

  const auto         spec = alcedo::ResolveEditorAdjustmentField(field_key);
  const OperatorType op   = spec.has_value() ? spec->operator_type : OperatorType::UNKNOWN;
  out.display_name        = DisplayName(op);
  out.icon_key            = IconResource(op);

  nlohmann::json after    = nlohmann::json::object();
  nlohmann::json before   = nlohmann::json::object();
  if (!after_value_json.empty()) {
    try {
      after = nlohmann::json::parse(after_value_json);
    } catch (...) {
    }
  }
  if (!before_value_json.empty()) {
    try {
      before = nlohmann::json::parse(before_value_json);
    } catch (...) {
    }
  }
  if (!after.is_object()) after = nlohmann::json::object();
  if (!before.is_object()) before = nlohmann::json::object();
  const CommitSummary summary = BuildSummary(op, after, before);
  out.before_text             = summary.before_text;
  out.after_text              = summary.after_text;
  out.delta_text              = summary.delta_text;

  // A disabled operator is a meaningful state change even when the numeric
  // value is unchanged; surface it so the card never reads as a no-op.
  if (!after_enabled && out.after_text.isEmpty()) {
    out.after_text = before_enabled ? QStringLiteral("Off") : QStringLiteral("Off");
    out.delta_text = before_enabled ? QStringLiteral("Enabled \u2192 Off") : QStringLiteral("Off");
  } else if (after_enabled && !before_enabled && out.delta_text.isEmpty()) {
    out.after_text = QStringLiteral("On");
    out.delta_text = QStringLiteral("Off \u2192 On");
  }
  if (out.delta_text.isEmpty() && !out.after_text.isEmpty()) {
    out.delta_text = out.before_text.isEmpty()
                         ? out.after_text
                         : QStringLiteral("%1 \u2192 %2").arg(out.before_text, out.after_text);
  }
  return out;
}

}  // namespace alcedo::ui
