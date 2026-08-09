//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_geometry_math.hpp"

#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <cmath>

#include "ui/alcedo_main/editor_support/modules/geometry.hpp"

namespace alcedo::ui {
namespace {

auto IsPositiveFinite(double value) -> bool {
  return std::isfinite(value) && value > static_cast<double>(geometry::kCropAspectMinValue);
}

auto ToRatioVariantList(const std::array<float, 2>& ratio) -> QVariantList {
  return {QVariant::fromValue(static_cast<double>(ratio[0])),
          QVariant::fromValue(static_cast<double>(ratio[1]))};
}

}  // namespace

EditorGeometryMath::EditorGeometryMath(QObject* parent) : QObject(parent) {}

auto EditorGeometryMath::aspectPresets() const -> QVariantList {
  QVariantList result;
  for (const auto& option : geometry::CropAspectPresetOptions()) {
    QVariantMap entry;
    entry.insert(QStringLiteral("value"), QString::fromUtf8(option.id_));
    entry.insert(QStringLiteral("label"), QString::fromUtf8(option.label_));
    entry.insert(QStringLiteral("width"), static_cast<double>(option.width_));
    entry.insert(QStringLiteral("height"), static_cast<double>(option.height_));
    result.push_back(entry);
  }
  return result;
}

QVariantList EditorGeometryMath::clampCropRect(double x, double y, double width,
                                               double height) const {
  const auto finite_or = [](double value, double fallback) {
    return std::isfinite(value) ? static_cast<float>(value) : static_cast<float>(fallback);
  };
  return ToRectVariantList(geometry::ClampCropRect(finite_or(x, 0.0), finite_or(y, 0.0),
                                                   finite_or(width, 1.0), finite_or(height, 1.0)));
}

QVariantList EditorGeometryMath::maxAspectCropRect(double image_aspect, double aspect_ratio) const {
  const float source_aspect =
      IsPositiveFinite(image_aspect) ? static_cast<float>(image_aspect) : 1.0f;
  const float crop_aspect =
      IsPositiveFinite(aspect_ratio) ? static_cast<float>(aspect_ratio) : 1.0f;
  return ToRectVariantList(geometry::MakeMaxAspectCropRect(source_aspect, crop_aspect));
}

QVariantList EditorGeometryMath::resizeAspectCropRect(double x, double y, double width,
                                                      double height, double image_aspect,
                                                      double aspect_ratio,
                                                      bool   use_width_driver) const {
  const float source_aspect =
      IsPositiveFinite(image_aspect) ? static_cast<float>(image_aspect) : 1.0f;
  const float crop_aspect =
      IsPositiveFinite(aspect_ratio) ? static_cast<float>(aspect_ratio) : 1.0f;
  return ToRectVariantList(geometry::ResizeAspectRectAroundCenter(
      static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
      static_cast<float>(height), source_aspect, crop_aspect, use_width_driver));
}

double EditorGeometryMath::aspectRatio(double width, double height) const {
  const auto ratio =
      geometry::AspectRatioFromSize(static_cast<float>(width), static_cast<float>(height));
  return ratio.has_value() ? static_cast<double>(*ratio) : 1.0;
}

bool EditorGeometryMath::hasLockedAspect(const QString& preset, double width, double height) const {
  const auto parsed = geometry::ParseCropAspectPreset(preset.toStdString());
  return parsed.has_value() &&
         geometry::HasLockedAspect(*parsed, static_cast<float>(width), static_cast<float>(height));
}

QVariantList EditorGeometryMath::presetRatio(const QString& preset) const {
  const auto parsed = geometry::ParseCropAspectPreset(preset.toStdString());
  if (!parsed.has_value()) {
    return {};
  }
  const auto ratio = geometry::CropAspectPresetRatio(*parsed);
  return ratio.has_value() ? ToRatioVariantList(*ratio) : QVariantList{};
}

QVariantList EditorGeometryMath::ToRectVariantList(const std::array<float, 4>& rect) {
  QVariantList result;
  result.reserve(4);
  for (const float value : rect) {
    result.push_back(static_cast<double>(value));
  }
  return result;
}

}  // namespace alcedo::ui
