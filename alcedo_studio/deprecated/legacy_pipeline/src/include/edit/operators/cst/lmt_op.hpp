//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <OpenColorIO/OpenColorIO.h>
#include <OpenColorIO/OpenColorTypes.h>

#include <filesystem>
#include <optional>

#include "edit/operators/op_base.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {
namespace OCIO = OCIO_NAMESPACE;

/**
 * @brief An operator to apply ACES Look Modification Transform (LMT) using OpenColorIO
 *
 */
class OCIO_LMT_Transform_Op : public OperatorBase<OCIO_LMT_Transform_Op> {
 private:
  std::filesystem::path        lmt_path_;
  // OCIO::ConstConfigRcPtr       config_;

  // OCIO::ConstCPUProcessorRcPtr cpu_processor_;
  // OCIO::ConstGPUProcessorRcPtr gpu_processor_;

 public:
  static constexpr PriorityLevel     priority_level_    = 3;
  // DO NOT USE THIS
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::Output_Transform;
  static constexpr std::string_view  canonical_name_    = "OCIO LMT";
  static constexpr std::string_view  script_name_       = "ocio_lmt";
  static constexpr OperatorType      operator_type_     = OperatorType::LMT;

  OCIO_LMT_Transform_Op()                               = delete;
  OCIO_LMT_Transform_Op(std::filesystem::path& lmt_path);
  OCIO_LMT_Transform_Op(const nlohmann::json& params);

  void Apply(std::shared_ptr<ImageBuffer> input) override;
  void ApplyGPU(std::shared_ptr<ImageBuffer> input) override;
  auto GetParams() const -> nlohmann::json override;
  void SetParams(const nlohmann::json& params) override;

  void SetGlobalParams(OperatorParams& params) const override;
  void EnableGlobalParams(OperatorParams& params, bool enable) override;
};

}  // namespace alcedo