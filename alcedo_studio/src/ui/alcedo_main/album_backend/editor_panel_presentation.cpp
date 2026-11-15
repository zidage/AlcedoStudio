//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_panel_presentation.hpp"

#include <QString>
#include <QVariant>
#include <QVariantList>
#include <type_traits>
#include <variant>
#include <vector>

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
          // `path`/`ocio_lmt` carry a file reference's path; official and library references
          // are resolved to their current file by the LUT library model (loadSelection).
          QString kind = QStringLiteral("none");
          QString path;
          if (const auto* file = std::get_if<alcedo::FileLutReference>(&value.reference)) {
            kind = QStringLiteral("file");
            path = String(file->path);
          } else if (const auto* official =
                         std::get_if<alcedo::OfficialLutReference>(&value.reference)) {
            kind = QStringLiteral("official");
            map.insert(QStringLiteral("packageId"), String(official->package_id));
            map.insert(QStringLiteral("lutId"), String(official->lut_id));
          } else if (const auto* library =
                         std::get_if<alcedo::LibraryLutReference>(&value.reference)) {
            kind = QStringLiteral("library");
            map.insert(QStringLiteral("libraryPath"), String(library->relative_path));
          }
          map.insert(QStringLiteral("referenceKind"), kind);
          map.insert(QStringLiteral("ocio_lmt"), path);
          map.insert(QStringLiteral("path"), path);
          map.insert(QStringLiteral("lutName"), String(value.display_name));
          map.insert(QStringLiteral("strength"), value.strength);
          map.insert(QStringLiteral("input_encoding"), String(value.input_encoding));
          map.insert(QStringLiteral("output_encoding"), String(value.output_encoding));
        } else if constexpr (std::is_same_v<T, alcedo::EditorPanelCurveValue>) {
          const auto point_list = [](const std::vector<alcedo::CurvePoint>& curve_points) {
            QVariantList points;
            points.reserve(static_cast<int>(curve_points.size()));
            for (const auto& point : curve_points) {
              QVariantMap row;
              row.insert(QStringLiteral("x"), point.x);
              row.insert(QStringLiteral("y"), point.y);
              points.push_back(row);
            }
            return points;
          };
          QVariantMap curve;
          curve.insert(QStringLiteral("points"), point_list(value.points));
          curve.insert(QStringLiteral("red"), point_list(value.red));
          curve.insert(QStringLiteral("green"), point_list(value.green));
          curve.insert(QStringLiteral("blue"), point_list(value.blue));
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
          wheels.insert(QStringLiteral("offset"), ControlMap(value.offset));
          map.insert(QStringLiteral("color_wheel"), wheels);
        } else if constexpr (std::is_same_v<T, alcedo::EditorPanelColorTempValue>) {
          QVariantMap temp;
          temp.insert(QStringLiteral("mode"), String(value.mode));
          temp.insert(QStringLiteral("custom_cct"), value.custom_cct);
          temp.insert(QStringLiteral("custom_tint"), value.custom_tint);
          temp.insert(QStringLiteral("as_shot_cct"), value.as_shot_cct);
          temp.insert(QStringLiteral("as_shot_tint"), value.as_shot_tint);
          map.insert(QStringLiteral("color_temp"), temp);
        } else if constexpr (std::is_same_v<T, alcedo::EditorPanelGradeWhiteBalanceValue>) {
          QVariantMap white_balance;
          white_balance.insert(QStringLiteral("temperature"), value.temperature);
          white_balance.insert(QStringLiteral("tint"), value.tint);
          map.insert(QStringLiteral("grade_white_balance"), white_balance);
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
          QVariantMap film_print;
          film_print.insert(QStringLiteral("negative"), String(value.film_print.negative));
          film_print.insert(QStringLiteral("print_material"),
                            String(value.film_print.print_material));
          film_print.insert(QStringLiteral("scene_light"), String(value.film_print.scene_light));
          film_print.insert(QStringLiteral("light_spread"), value.film_print.light_spread);
          film_print.insert(QStringLiteral("halation"), value.film_print.halation);
          film_print.insert(QStringLiteral("enlarger_flare"), value.film_print.enlarger_flare);
          // Negative Scan (issue 661): every key, also at its default, so the echo of a write
          // gives each control its stored value.
          const auto& scan = value.film_print.scan;
          film_print.insert(QStringLiteral("auto"), String(scan.auto_colour));
          film_print.insert(QStringLiteral("auto_strength"), scan.auto_strength);
          film_print.insert(QStringLiteral("auto_density"), scan.auto_density);
          film_print.insert(QStringLiteral("scan_contrast"), scan.contrast);
          film_print.insert(QStringLiteral("scan_exposure_stops"), scan.exposure_stops);
          film_print.insert(QStringLiteral("cyan"), scan.cyan);
          film_print.insert(QStringLiteral("magenta"), scan.magenta);
          film_print.insert(QStringLiteral("yellow"), scan.yellow);
          film_print.insert(QStringLiteral("black"), scan.black);
          film_print.insert(QStringLiteral("scanner_red_nm"), scan.scanner_red_nm);
          film_print.insert(QStringLiteral("scanner_green_nm"), scan.scanner_green_nm);
          film_print.insert(QStringLiteral("scanner_blue_nm"), scan.scanner_blue_nm);
          film_print.insert(QStringLiteral("scanner_width_nm"), scan.scanner_width_nm);
          film_print.insert(QStringLiteral("scanner_light"), String(scan.scanner_light));
          QVariantMap odt;
          odt.insert(QStringLiteral("method"), String(value.method));
          odt.insert(QStringLiteral("encoding_space"), String(value.encoding_space));
          odt.insert(QStringLiteral("encoding_eotf"), String(value.encoding_eotf));
          odt.insert(QStringLiteral("limiting_space"), String(value.limiting_space));
          odt.insert(QStringLiteral("peak_luminance"), value.peak_luminance);
          odt.insert(QStringLiteral("open_drt"), open_drt);
          odt.insert(QStringLiteral("film_print"), film_print);
          odt.insert(QStringLiteral("film_as_shot_available"), value.film_as_shot_available);
          map.insert(QStringLiteral("odt"), odt);
        } else if constexpr (std::is_same_v<T, alcedo::EditorPanelLensValue>) {
          QVariantMap lens;
          lens.insert(QStringLiteral("enabled"), value.enabled);
          lens.insert(QStringLiteral("lens_maker"), String(value.lens_maker));
          lens.insert(QStringLiteral("lens_model"), String(value.lens_model));
          map.insert(QStringLiteral("lens_calib"), lens);
        } else if constexpr (std::is_same_v<T, alcedo::EditorPanelInputProfileValue>) {
          QVariantMap input;
          input.insert(QStringLiteral("raster"), value.raster);
          input.insert(QStringLiteral("profile_override"), String(value.profile_override));
          input.insert(QStringLiteral("profile_description"), String(value.profile_description));
          input.insert(QStringLiteral("origin"), String(value.origin));
          input.insert(QStringLiteral("referral"), String(value.referral));
          map.insert(QStringLiteral("input"), input);
        } else if constexpr (std::is_same_v<T, alcedo::EditorPanelCameraProfileFileValue>) {
          QVariantMap reference;
          reference.insert(QStringLiteral("raster"), value.raster);
          reference.insert(QStringLiteral("embedded_default"), value.embedded_default);
          reference.insert(QStringLiteral("selected"), value.selected);
          reference.insert(QStringLiteral("resolved"), value.resolved);
          reference.insert(QStringLiteral("has_tone_curve"), value.has_tone_curve);
          reference.insert(QStringLiteral("profile_tone"), value.profile_tone);
          reference.insert(QStringLiteral("camera_model"), String(value.camera_model));
          reference.insert(QStringLiteral("profile_name"), String(value.profile_name));
          map.insert(QStringLiteral("camera_profile_file"), reference);
        } else if constexpr (std::is_same_v<T, alcedo::EditorPanelFilmFormatValue>) {
          QVariantMap format;
          format.insert(QStringLiteral("film_format"), String(value.film_format));
          map.insert(QStringLiteral("film_format"), format);
        } else if constexpr (std::is_same_v<T, alcedo::EditorPanelGeometryValue>) {
          QVariantMap rect;
          rect.insert(QStringLiteral("x"), value.crop_rect.x);
          rect.insert(QStringLiteral("y"), value.crop_rect.y);
          rect.insert(QStringLiteral("w"), value.crop_rect.w);
          rect.insert(QStringLiteral("h"), value.crop_rect.h);
          QVariantMap crop;
          crop.insert(QStringLiteral("crop_rect"), rect);
          crop.insert(QStringLiteral("angle_degrees"), value.rotation_degrees);
          crop.insert(QStringLiteral("aspect_ratio_preset"), String(value.aspect_preset));
          QVariantMap aspect;
          aspect.insert(QStringLiteral("width"), value.aspect_ratio.width);
          aspect.insert(QStringLiteral("height"), value.aspect_ratio.height);
          crop.insert(QStringLiteral("aspect_ratio"), aspect);
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
