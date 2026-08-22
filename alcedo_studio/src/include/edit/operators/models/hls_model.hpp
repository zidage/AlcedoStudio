//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>

#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_model_base.hpp"

namespace alcedo {

inline constexpr int kHlsHueBinCount = 8;

struct HlsVec3 {
  float h = 0.0f;
  float l = 0.0f;
  float s = 0.0f;
};

struct HlsPayload {
  std::array<float, kHlsHueBinCount>   hue_bins{0.0f, 45.0f, 90.0f, 135.0f,
                                                180.0f, 225.0f, 270.0f, 315.0f};
  std::array<HlsVec3, kHlsHueBinCount> hls_adj_table{};
  std::array<float, kHlsHueBinCount>   h_range_table{45.0f, 45.0f, 45.0f, 45.0f,
                                                     45.0f, 45.0f, 45.0f, 45.0f};
  HlsVec3                              target_hls{0.0f, 0.5f, 1.0f};
  HlsVec3                              hls_adj{};
  float                                h_range = 45.0f;
  float                                l_range = 0.1f;
  float                                s_range = 0.1f;
};

enum class HlsDirty : std::uint32_t {
  None  = 0,
  Table = 1U << 0,
  All   = Table,
};

/**
 * @brief Eight-bin HLS adjustment tables. Default tables are zero (identity).
 */
class HlsModel final : public OperatorModelBase<HlsModel, HlsPayload, HlsDirty> {
 public:
  static auto TypeId() -> const OperatorTypeId& { return type_ids::Hls(); }
  static constexpr std::string_view kInstanceSuffix = "hls";

  [[nodiscard]] auto IsDefault() const -> bool override;

  [[nodiscard]] auto ToJson() const -> nlohmann::json override;
  void               LoadJson(const nlohmann::json& json) override;
};

}  // namespace alcedo
