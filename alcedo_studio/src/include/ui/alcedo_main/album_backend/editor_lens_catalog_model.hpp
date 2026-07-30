//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include "ui/alcedo_main/editor_dialog/modules/lens_calib.hpp"

namespace alcedo::ui {

/// Read-only QML view over the shared lens catalog used by the legacy editor.
class EditorLensCatalogModel : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList brands READ brands CONSTANT)
  Q_PROPERTY(QString statusText READ statusText CONSTANT)
  Q_PROPERTY(QString defaultParamsJson READ defaultParamsJson CONSTANT)

 public:
  explicit EditorLensCatalogModel(QObject* parent = nullptr);

  [[nodiscard]] auto brands() const -> QVariantList { return brands_; }
  [[nodiscard]] auto statusText() const -> QString { return status_text_; }
  [[nodiscard]] auto defaultParamsJson() const -> QString { return default_params_json_; }
  Q_INVOKABLE QVariantList modelsForBrand(const QString& brand) const;

 private:
  lens_calib::LensCatalog catalog_;
  QVariantList brands_;
  QString status_text_;
  QString default_params_json_;
};

}  // namespace alcedo::ui
