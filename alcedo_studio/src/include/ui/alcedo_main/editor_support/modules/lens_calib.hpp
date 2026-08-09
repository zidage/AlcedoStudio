//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace alcedo::ui::lens_calib {

struct LensCatalog {
  std::vector<std::string>                        brands_{};
  std::map<std::string, std::vector<std::string>> models_by_brand_{};
};

void SortAndUniqueStrings(std::vector<std::string>* values);
auto ResolveLensCatalogPath() -> std::filesystem::path;
auto LoadLensCatalog() -> LensCatalog;

}  // namespace alcedo::ui::lens_calib
