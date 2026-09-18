//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_model_base.hpp"

namespace alcedo {

inline constexpr int kHlsHueBinCount = 8;

struct HlsVec3 {
  float h = 0.0f;
  float l = 0.0f;
  float s = 0.0f;
};

inline auto operator==(const HlsVec3& lhs, const HlsVec3& rhs) -> bool {
  return lhs.h == rhs.h && lhs.l == rhs.l && lhs.s == rhs.s;
}

struct HlsPayload {
  std::array<float, kHlsHueBinCount>   hue_bins{0.0f,   45.0f,  90.0f,  135.0f,
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
 * @brief Focused HLS table update. Omitted fields retain their current values.
 */
struct HlsUpdate {
  std::optional<std::array<float, kHlsHueBinCount>>   hue_bins;
  std::optional<std::array<HlsVec3, kHlsHueBinCount>> hls_adj_table;
  std::optional<std::array<float, kHlsHueBinCount>>   h_range_table;
  std::optional<HlsVec3>                              target_hls;
  std::optional<HlsVec3>                              hls_adj;
  std::optional<float>                                h_range;
  std::optional<float>                                l_range;
  std::optional<float>                                s_range;
};

/**
 * @brief Eight-bin HLS adjustment tables. Default tables are zero (identity).
 */
class HlsModel final : public OperatorModelBase<HlsModel, HlsPayload, HlsDirty> {
 public:
  static auto                       TypeId() -> const OperatorTypeId& { return type_ids::Hls(); }
  static constexpr std::string_view kInstanceSuffix = "hls";

  [[nodiscard]] auto                IsDefault() const -> bool override;

  /**
   * @brief Read individual HLS fields through the owning Model lock.
   */
  [[nodiscard]] auto                HueBins() const -> std::array<float, kHlsHueBinCount>;
  [[nodiscard]] auto                AdjustmentTable() const -> std::array<HlsVec3, kHlsHueBinCount>;
  [[nodiscard]] auto                HueRangeTable() const -> std::array<float, kHlsHueBinCount>;
  [[nodiscard]] auto                TargetHls() const -> HlsVec3;
  [[nodiscard]] auto                Adjustment() const -> HlsVec3;
  [[nodiscard]] auto                HueRange() const -> float;
  [[nodiscard]] auto                LightnessRange() const -> float;
  [[nodiscard]] auto                SaturationRange() const -> float;

  /**
   * @brief Apply validated HLS fields atomically and mark dirty only when a field changes.
   */
  void                              ApplyUpdate(HlsUpdate update);

  [[nodiscard]] auto                ToJson() const -> nlohmann::json override;
  void                              LoadJson(const nlohmann::json& json) override;
};

}  // namespace alcedo
