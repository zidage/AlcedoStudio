//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>

#include "json.hpp"

namespace alcedo::json_util {

inline auto ReadFloat(const nlohmann::json& json, const char* key, float fallback) -> float {
  if (!json.contains(key) || !json[key].is_number()) {
    return fallback;
  }
  return json[key].get<float>();
}

inline auto ReadBool(const nlohmann::json& json, const char* key, bool fallback) -> bool {
  if (!json.contains(key) || !json[key].is_boolean()) {
    return fallback;
  }
  return json[key].get<bool>();
}

inline auto ReadString(const nlohmann::json& json, const char* key, std::string fallback)
    -> std::string {
  if (!json.contains(key) || !json[key].is_string()) {
    return fallback;
  }
  return json[key].get<std::string>();
}

}  // namespace alcedo::json_util
