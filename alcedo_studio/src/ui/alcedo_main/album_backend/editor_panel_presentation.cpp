//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_panel_presentation.hpp"

#include <QString>
#include <QVariant>
#include <QVariantList>
#include <type_traits>

namespace alcedo::ui {
namespace {

auto String(const std::string& value) -> QString { return QString::fromStdString(value); }

auto ControlMap(const alcedo::ColorWheelControl& control) -> QVariantMap {
  QVariantMap disc;
  disc.insert(QStringLiteral("x"), control.disc.x);
  disc.insert(QStringLiteral("y"), control.disc.y);
  QVariantMap map;
  map.insert(QStringLiteral("disc"), disc);
  map.insert(QStringLiteral("strength"), control.strength);
  map.insert(QStringLiteral("luminance_offset"), control.luminance_offset);
  return map;
}

auto FieldMap(const alcedo::EditorPanelFieldPresentation& field) -> QVariantMap {
  QVariantMap map;
  std::visit(
      [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, alcedo::EditorPanelScalarValue>) {
          map.insert(String(value.display_key), value.value);
        } else if constexpr (std::is_same_v<T, alcedo::EditorPanelNestedScalarValue>) {
          QVariantMap inner;
          inner.insert(String(value.value_key), value.value);
          map.insert(String(value.object_key), inner);
        } else if constexpr (std::is_same_v<T, alcedo::EditorPanelLutValue>) {
          const auto path = String(value.cube_path);
          map.insert(QStringLiteral("ocio_lmt"), path);
          map.insert(QStringLiteral("path"), path);
        } else if constexpr (std::is_same_v<T, alcedo::EditorPanelCurveValue>) {
          QVariantList points;
          points.reserve(static_cast<int>(value.points.size()));
          for (const auto& point : value.points) {
            QVariantMap row;
            row.insert(QStringLiteral("x"), point.x);
            row.insert(QStringLiteral("y"), point.y);
            points.push_back(row);
          }
          QVariantMap curve;
          curve.insert(QStringLiteral("points"), points);
          map.insert(QStringLiteral("curve"), curve);
        } else if constexpr (std::is_same_v<T, alcedo::EditorPanelHlsValue>) {
          QVariantList table;
          QVariantList ranges;
          table.reserve(alcedo::kHlsHueBinCount);
          ranges.reserve(alcedo::kHlsHueBinCount);
          for (int i = 0; i < alcedo::kHlsHueBinCount; ++i) {
            const auto& row = value.hls_adj_table[static_cast<std::size_t>(i)];
            table.push_back(QVariantList{row.h, row.l, row.s});
            ranges.push_back(value.h_range_table[static_cast<std::size_t>(i)]);
          }
          QVariantMap hls;
          hls.insert(QStringLiteral("hls_adj_table"), table);
          hls.insert(QStringLiteral("h_range_table"), ranges);
          hls.insert(QStringLiteral("target_hls"),
                     QVariantList{value.target_hls.h, value.target_hls.l, value.target_hls.s});
          map.insert(QStringLiteral("HLS"), hls);
        } else if constexpr (std::is_same_v<T, alcedo::EditorPanelColorWheelValue>) {
          QVariantMap wheels;
          wheels.insert(QStringLiteral("lift"), ControlMap(value.lift));
          wheels.insert(QStringLiteral("gamma"), ControlMap(value.gamma));
          wheels.insert(QStringLiteral("gain"), ControlMap(value.gain));
          map.insert(QStringLiteral("color_wheel"), wheels);
        } else if constexpr (std::is_same_v<T, alcedo::EditorPanelColorTempValue>) {
          QVariantMap temp;
          temp.insert(QStringLiteral("mode"), String(value.mode));
          temp.insert(QStringLiteral("custom_cct"), value.custom_cct);
          temp.insert(QStringLiteral("custom_tint"), value.custom_tint);
          temp.insert(QStringLiteral("as_shot_cct"), value.as_shot_cct);
          temp.insert(QStringLiteral("as_shot_tint"), value.as_shot_tint);
          map.insert(QStringLiteral("color_temp"), temp);
        } else if constexpr (std::is_same_v<T, alcedo::EditorPanelRawDecodeValue>) {
          QVariantMap raw;
          raw.insert(QStringLiteral("method"), String(value.method));
          raw.insert(QStringLiteral("highlights_reconstruct"), value.highlights_reconstruct);
          map.insert(QStringLiteral("raw"), raw);
        } else if constexpr (std::is_same_v<T, alcedo::EditorPanelOdtValue>) {
          QVariantMap open_drt;
          open_drt.insert(QStringLiteral("look_preset"), String(value.look_preset));
          open_drt.insert(QStringLiteral("tonescale_preset"), String(value.tonescale_preset));
          open_drt.insert(QStringLiteral("creative_white"), String(value.creative_white));
          QVariantMap odt;
          odt.insert(QStringLiteral("method"), String(value.method));
          odt.insert(QStringLiteral("encoding_space"), String(value.encoding_space));
          odt.insert(QStringLiteral("encoding_eotf"), String(value.encoding_eotf));
          odt.insert(QStringLiteral("limiting_space"), String(value.limiting_space));
          odt.insert(QStringLiteral("peak_luminance"), value.peak_luminance);
          odt.insert(QStringLiteral("open_drt"), open_drt);
          map.insert(QStringLiteral("odt"), odt);
        } else if constexpr (std::is_same_v<T, alcedo::EditorPanelLensValue>) {
          QVariantMap lens;
          lens.insert(QStringLiteral("enabled"), value.enabled);
          map.insert(QStringLiteral("lens_calib"), lens);
        } else if constexpr (std::is_same_v<T, alcedo::EditorPanelGeometryValue>) {
          QVariantMap rect;
          rect.insert(QStringLiteral("x"), value.crop_rect.x);
          rect.insert(QStringLiteral("y"), value.crop_rect.y);
          rect.insert(QStringLiteral("w"), value.crop_rect.w);
          rect.insert(QStringLiteral("h"), value.crop_rect.h);
          QVariantMap crop;
          crop.insert(QStringLiteral("crop_rect"), rect);
          crop.insert(QStringLiteral("angle_degrees"), value.rotation_degrees);
          crop.insert(QStringLiteral("expand_to_fit"), value.expand_to_fit);
          map.insert(QStringLiteral("crop_rotate"), crop);
        }
      },
      field.value);
  return map;
}

}  // namespace

auto PanelProjectionToVariantMap(const alcedo::EditorPanelProjection& projection) -> QVariantMap {
  QVariantMap map;
  for (const auto& field : projection.fields) {
    map.insert(QString::fromStdString(field.field_key), FieldMap(field));
  }
  return map;
}

auto ApplyPanelProjectionToSnapshotMap(const alcedo::EditorPanelProjection& projection,
                                       std::uint64_t session_generation, QVariantMap* dest) -> bool {
  if (dest == nullptr) {
    return false;
  }
  if (!alcedo::EditorPanelProjectionIsCurrent(projection, session_generation)) {
    return false;
  }
  const auto incoming = PanelProjectionToVariantMap(projection);
  if (projection.fields.empty()) {
    if (dest->isEmpty()) {
      return false;
    }
    dest->clear();
    return true;
  }
  bool changed = false;
  for (auto it = incoming.constBegin(); it != incoming.constEnd(); ++it) {
    if (dest->value(it.key()) != it.value()) {
      dest->insert(it.key(), it.value());
      changed = true;
    }
  }
  return changed;
}

}  // namespace alcedo::ui
