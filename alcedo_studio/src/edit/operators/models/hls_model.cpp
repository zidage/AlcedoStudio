//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/models/hls_model.hpp"

#include "edit/operators/models/json_read.hpp"

namespace alcedo {

namespace {

auto TablesAreIdentity(const HlsPayload& payload) -> bool {
  for (const auto& adj : payload.hls_adj_table) {
    if (adj.h != 0.0f || adj.l != 0.0f || adj.s != 0.0f) {
      return false;
    }
  }
  return payload.hls_adj.h == 0.0f && payload.hls_adj.l == 0.0f && payload.hls_adj.s == 0.0f;
}

auto VecToJson(const HlsVec3& vec) -> nlohmann::json { return nlohmann::json::array({vec.h, vec.l, vec.s}); }

auto VecFromJson(const nlohmann::json& json, HlsVec3 fallback) -> HlsVec3 {
  if (!json.is_array() || json.size() < 3) {
    return fallback;
  }
  HlsVec3 vec;
  vec.h = json[0].is_number() ? json[0].get<float>() : fallback.h;
  vec.l = json[1].is_number() ? json[1].get<float>() : fallback.l;
  vec.s = json[2].is_number() ? json[2].get<float>() : fallback.s;
  return vec;
}

}  // namespace

auto HlsModel::IsDefault() const -> bool {
  return Read([](const HlsPayload& payload) { return TablesAreIdentity(payload); });
}

auto HlsModel::ToJson() const -> nlohmann::json {
  const auto     payload = PayloadCopy();
  nlohmann::json hue_bins = nlohmann::json::array();
  nlohmann::json adj_table = nlohmann::json::array();
  nlohmann::json range_table = nlohmann::json::array();
  for (int i = 0; i < kHlsHueBinCount; ++i) {
    hue_bins.push_back(payload.hue_bins[static_cast<std::size_t>(i)]);
    adj_table.push_back(VecToJson(payload.hls_adj_table[static_cast<std::size_t>(i)]));
    range_table.push_back(payload.h_range_table[static_cast<std::size_t>(i)]);
  }
  return {{"hue_bins", std::move(hue_bins)},
          {"hls_adj_table", std::move(adj_table)},
          {"h_range_table", std::move(range_table)},
          {"target_hls", VecToJson(payload.target_hls)},
          {"hls_adj", VecToJson(payload.hls_adj)},
          {"h_range", payload.h_range},
          {"l_range", payload.l_range},
          {"s_range", payload.s_range}};
}

void HlsModel::LoadJson(const nlohmann::json& json) {
  Mutate(HlsDirty::Table, [&json](HlsPayload& payload) {
    if (json.contains("hue_bins") && json["hue_bins"].is_array()) {
      const auto& bins = json["hue_bins"];
      for (int i = 0; i < kHlsHueBinCount && i < static_cast<int>(bins.size()); ++i) {
        if (bins[i].is_number()) {
          payload.hue_bins[static_cast<std::size_t>(i)] = bins[i].get<float>();
        }
      }
    }
    if (json.contains("hls_adj_table") && json["hls_adj_table"].is_array()) {
      const auto& table = json["hls_adj_table"];
      for (int i = 0; i < kHlsHueBinCount && i < static_cast<int>(table.size()); ++i) {
        payload.hls_adj_table[static_cast<std::size_t>(i)] =
            VecFromJson(table[i], payload.hls_adj_table[static_cast<std::size_t>(i)]);
      }
    }
    if (json.contains("h_range_table") && json["h_range_table"].is_array()) {
      const auto& table = json["h_range_table"];
      for (int i = 0; i < kHlsHueBinCount && i < static_cast<int>(table.size()); ++i) {
        if (table[i].is_number()) {
          payload.h_range_table[static_cast<std::size_t>(i)] = table[i].get<float>();
        }
      }
    }
    if (json.contains("target_hls")) {
      payload.target_hls = VecFromJson(json["target_hls"], payload.target_hls);
    }
    if (json.contains("hls_adj")) {
      payload.hls_adj = VecFromJson(json["hls_adj"], payload.hls_adj);
    }
    payload.h_range = json_util::ReadFloat(json, "h_range", payload.h_range);
    payload.l_range = json_util::ReadFloat(json, "l_range", payload.l_range);
    payload.s_range = json_util::ReadFloat(json, "s_range", payload.s_range);
  });
}

}  // namespace alcedo
