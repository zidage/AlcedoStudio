//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <opencv2/core.hpp>

#include <cstdint>
#include <string>

#include "edit/operators/op_base.hpp"

namespace alcedo {
class ColorTempOp : public OperatorBase<ColorTempOp> {
 private:
  ColorTempMode mode_         = ColorTempMode::AS_SHOT;
  float         custom_cct_   = 6500.0f;
  float         custom_tint_  = 0.0f;
  mutable float resolved_cct_ = 6500.0f;
  mutable float resolved_tint_ = 0.0f;

  static auto   ParseMode(const std::string& mode) -> ColorTempMode;
  static auto   ModeToString(ColorTempMode mode) -> std::string;

  // Resolve runtime camera → XYZ/AP1 matrices from current RAW context.
  // Called eagerly from SetGlobalParams; not exposed publicly.
  void ResolveRuntime(OperatorParams& params) const;

 public:
  static constexpr PriorityLevel     priority_level_    = 0;
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::To_WorkingSpace;
  static constexpr OperatorType      operator_type_     = OperatorType::COLOR_TEMP;
  static constexpr std::string_view  canonical_name_    = "Color Temperature";
  static constexpr std::string_view  script_name_       = "color_temp";

  ColorTempOp()                                          = default;
  explicit ColorTempOp(const nlohmann::json& params);

  void Apply(std::shared_ptr<ImageBuffer> input) override;
  void ApplyGPU(std::shared_ptr<ImageBuffer> input) override;

  auto GetParams() const -> nlohmann::json override;
  void SetParams(const nlohmann::json& params) override;
  void SetGlobalParams(OperatorParams& params) const override;
  void EnableGlobalParams(OperatorParams& params, bool enable) override;

  [[nodiscard]] auto DetectMergeConflict(const nlohmann::json& current,
                                         const nlohmann::json& incoming) const -> bool override;
  [[nodiscard]] auto MergeParams(const nlohmann::json& current, const nlohmann::json& incoming,
                                 OperatorMergeChoice choice) const -> nlohmann::json override;
};
}  // namespace alcedo
