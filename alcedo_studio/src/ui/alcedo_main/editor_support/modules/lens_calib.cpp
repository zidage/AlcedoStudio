//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_support/modules/lens_calib.hpp"

#include <QCoreApplication>
#include <algorithm>
#include <fstream>
#include <json.hpp>

namespace alcedo::ui::lens_calib {

void SortAndUniqueStrings(std::vector<std::string>* values) {
  if (!values) {
    return;
  }
  std::sort(values->begin(), values->end());
  values->erase(std::unique(values->begin(), values->end()), values->end());
}

auto ResolveLensCatalogPath() -> std::filesystem::path {
  const std::filesystem::path app_dir(QCoreApplication::applicationDirPath().toStdWString());
  const std::vector<std::filesystem::path> candidates = {
      app_dir / "lens_calib" / "lens_catalog.json",
      app_dir / "config" / "lens_calib" / "lens_catalog.json",
      std::filesystem::path(CONFIG_PATH) / "lens_calib" / "lens_catalog.json",
      std::filesystem::path("src/config/lens_calib/lens_catalog.json"),
      std::filesystem::path("alcedo/src/config/lens_calib/lens_catalog.json"),
  };

  for (const auto& path : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec && std::filesystem::is_regular_file(path, ec) &&
        !ec) {
      return path;
    }
  }
  return {};
}

auto LoadLensCatalog() -> LensCatalog {
  LensCatalog catalog;

  const auto  path = ResolveLensCatalogPath();
  if (path.empty()) {
    return catalog;
  }

  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return catalog;
  }

  nlohmann::json j;
  try {
    in >> j;
  } catch (...) {
    return catalog;
  }

  if (!j.is_object() || !j.contains("brands") || !j["brands"].is_object()) {
    return catalog;
  }

  for (auto it = j["brands"].begin(); it != j["brands"].end(); ++it) {
    const std::string brand = it.key();
    if (brand.empty() || !it.value().is_array()) {
      continue;
    }
    std::vector<std::string> models;
    for (const auto& model_json : it.value()) {
      if (!model_json.is_string()) {
        continue;
      }
      const std::string model = model_json.get<std::string>();
      if (!model.empty()) {
        models.push_back(model);
      }
    }
    SortAndUniqueStrings(&models);
    if (models.empty()) {
      continue;
    }
    catalog.models_by_brand_[brand] = std::move(models);
    catalog.brands_.push_back(brand);
  }

  SortAndUniqueStrings(&catalog.brands_);
  return catalog;
}

}  // namespace alcedo::ui::lens_calib
