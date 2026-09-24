//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <OpenColorIO/OpenColorIO.h>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace alcedo::ocio_config {
namespace OCIO = OCIO_NAMESPACE;

inline auto PathToUtf8(const std::filesystem::path& path) -> std::string {
  const auto utf8 = path.generic_u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

inline auto DefaultConfigPath() -> std::filesystem::path {
#ifdef CONFIG_PATH
  return std::filesystem::path(CONFIG_PATH) / "OCIO" / "config.ocio";
#else
  return {};
#endif
}

inline auto LoadBundledConfig() -> OCIO::ConstConfigRcPtr {
  const auto config_path = DefaultConfigPath();
  if (config_path.empty()) {
    throw std::runtime_error("OCIO config path is unavailable in this build.");
  }
  if (!std::filesystem::is_regular_file(config_path)) {
    throw std::runtime_error("OCIO config file does not exist: " + PathToUtf8(config_path));
  }
  return OCIO::Config::CreateFromFile(PathToUtf8(config_path).c_str());
}

}  // namespace alcedo::ocio_config
