//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_lens_catalog_model.hpp"

#include <QString>
#include <QVariantMap>

#include "edit/pipeline/default_pipeline_params.hpp"

namespace alcedo::ui {

EditorLensCatalogModel::EditorLensCatalogModel(QObject* parent) : QObject(parent) {
  catalog_ = lens_calib::LoadLensCatalog();
  for (const auto& brand : catalog_.brands_) {
    QVariantMap entry;
    entry.insert(QStringLiteral("value"), QString::fromStdString(brand));
    entry.insert(QStringLiteral("label"), QString::fromStdString(brand));
    brands_.push_back(entry);
  }

  status_text_ = brands_.isEmpty() ? tr("Lens catalog is unavailable")
                                   : tr("%1 lens brands available").arg(brands_.size());
  default_params_json_ =
      QString::fromStdString(pipeline_defaults::MakeDefaultLensCalibParams().dump());
}

QVariantList EditorLensCatalogModel::modelsForBrand(const QString& brand) const {
  const auto it = catalog_.models_by_brand_.find(brand.toStdString());
  if (it == catalog_.models_by_brand_.end()) {
    return {};
  }

  QVariantList result;
  for (const auto& model : it->second) {
    QVariantMap entry;
    entry.insert(QStringLiteral("value"), QString::fromStdString(model));
    entry.insert(QStringLiteral("label"), QString::fromStdString(model));
    result.push_back(entry);
  }
  return result;
}

}  // namespace alcedo::ui
