//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <OpenColorIO/OpenColorIO.h>
#include <OpenColorIO/OpenColorTypes.h>

#include <filesystem>
#include <optional>

#include "edit/operators/op_base.hpp"
#include "edit/operators/op_kernel.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {
namespace OCIO = OCIO_NAMESPACE;
class OCIO_ACES_Transform_Op : public OperatorBase<OCIO_ACES_Transform_Op> {
 public:
  enum class TransformType : uint32_t { To_WorkingSpace = 0, To_OutputSpace = 1 };

 private:
  TransformType                        transform_type_ = TransformType::To_WorkingSpace;
  std::string                          input_transform_;
  std::string                          output_transform_;
  bool                                 limit_     = false;
  bool                                 normalize_ = false;

  std::optional<std::filesystem::path> lmt_path_;

  // OCIO::ConstConfigRcPtr               config_;

  // OCIO::ConstCPUProcessorRcPtr         cpu_processor_;
  // OCIO::ConstGPUProcessorRcPtr         gpu_processor_;
  // OCIO::BakerRcPtr                     baker_;

  void                                 SetCSTProcessors(const char* input, const char* output);
  void                                 SetDisplayProcessors(const char* output);

 public:
  static constexpr PriorityLevel     priority_level_    = 2;
  // DO NOT USE THIS
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::Output_Transform;
  static constexpr std::string_view  canonical_name_    = "OCIO";
  static constexpr std::string_view  script_name_       = "ocio";
  static constexpr OperatorType      operator_type_     = OperatorType::CST;
  OCIO_ACES_Transform_Op()                              = delete;
  OCIO_ACES_Transform_Op(const std::string& input, const std::string& output);
  OCIO_ACES_Transform_Op(const std::string& input, const std::string& output,
                         const char* config_path);
  OCIO_ACES_Transform_Op(std::filesystem::path& lmt_path);
  OCIO_ACES_Transform_Op(const nlohmann::json& params);

  void Apply(std::shared_ptr<ImageBuffer> input) override;
  void ApplyGPU(std::shared_ptr<ImageBuffer> input) override;
  auto ApplyLMT(ImageBuffer& input) -> ImageBuffer;
  auto GetParams() const -> nlohmann::json override;
  void SetParams(const nlohmann::json& params) override;

  void SetGlobalParams(OperatorParams& params) const override;
  void EnableGlobalParams(OperatorParams& params, bool enable) override;
};

}  // namespace alcedo