//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QVariantList>

#include <array>

namespace alcedo::ui {

/// QML adapter for the shared crop geometry equations.
///
/// The editor panel keeps its state in normalized coordinates, while the
/// implementation of aspect fitting and resizing remains in the same pure
/// module used by the legacy editor and the pipeline path.
class EditorGeometryMath : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList aspectPresets READ aspectPresets CONSTANT)

 public:
  explicit EditorGeometryMath(QObject* parent = nullptr);

  [[nodiscard]] auto aspectPresets() const -> QVariantList;

  Q_INVOKABLE QVariantList clampCropRect(double x, double y, double width, double height) const;
  Q_INVOKABLE QVariantList maxAspectCropRect(double image_aspect, double aspect_ratio) const;
  Q_INVOKABLE QVariantList resizeAspectCropRect(double x, double y, double width, double height,
                                                double image_aspect, double aspect_ratio,
                                                bool use_width_driver) const;
  Q_INVOKABLE double aspectRatio(double width, double height) const;
  Q_INVOKABLE bool hasLockedAspect(const QString& preset, double width, double height) const;
  Q_INVOKABLE QVariantList presetRatio(const QString& preset) const;

 private:
  static QVariantList ToRectVariantList(const std::array<float, 4>& rect);
};

}  // namespace alcedo::ui
