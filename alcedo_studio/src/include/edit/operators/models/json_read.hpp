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

inline auto ReadDouble(const nlohmann::json& json, const char* key, double fallback) -> double {
  if (!json.contains(key) || !json[key].is_number()) {
    return fallback;
  }
  return json[key].get<double>();
}

inline void ReadNumberArray(const nlohmann::json& json, const char* key, double* dest, int count) {
  if (dest == nullptr || count <= 0 || !json.contains(key) || !json[key].is_array()) {
    return;
  }
  const auto& values = json[key];
  for (int i = 0; i < count && i < static_cast<int>(values.size()); ++i) {
    if (values[i].is_number()) {
      dest[i] = values[i].get<double>();
    }
  }
}

inline void ReadNumberArray(const nlohmann::json& json, const char* key, float* dest, int count) {
  if (dest == nullptr || count <= 0 || !json.contains(key) || !json[key].is_array()) {
    return;
  }
  const auto& values = json[key];
  for (int i = 0; i < count && i < static_cast<int>(values.size()); ++i) {
    if (values[i].is_number()) {
      dest[i] = values[i].get<float>();
    }
  }
}

inline auto MakeJsonArray(const double* values, int count) -> nlohmann::json {
  nlohmann::json array = nlohmann::json::array();
  for (int i = 0; i < count; ++i) {
    array.push_back(values[i]);
  }
  return array;
}

inline auto MakeJsonArray(const float* values, int count) -> nlohmann::json {
  nlohmann::json array = nlohmann::json::array();
  for (int i = 0; i < count; ++i) {
    array.push_back(values[i]);
  }
  return array;
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
