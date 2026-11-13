//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/cst/cst_op.hpp"

#include <OpenColorIO/OpenColorIO.h>
#include <OpenColorIO/OpenColorTransforms.h>
#include <OpenColorIO/OpenColorTypes.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "edit/operators/operator_factory.hpp"
#include "edit/operators/utils/functions.hpp"
#include "image/image_buffer.hpp"
#include "json.hpp"
#include "type/size.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {

OCIO_ACES_Transform_Op::OCIO_ACES_Transform_Op(const std::string& input, const std::string& output)
    : input_transform_(input), output_transform_(output) {
  // config_ = ocio_config::LoadBundledConfig();
}

OCIO_ACES_Transform_Op::OCIO_ACES_Transform_Op(const std::string& input, const std::string& output,
                                               const char* config_path)
    : input_transform_(input), output_transform_(output) {
  // config_ = OCIO::Config::CreateFromFile(config_path);
}

OCIO_ACES_Transform_Op::OCIO_ACES_Transform_Op(std::filesystem::path& lmt_path)
    : input_transform_("ACES - ACEScct"), output_transform_("ACES - ACEScct"), lmt_path_(lmt_path) {
  // config_ = ocio_config::LoadBundledConfig();
}

OCIO_ACES_Transform_Op::OCIO_ACES_Transform_Op(const nlohmann::json& params) {
  // config_ = ocio_config::LoadBundledConfig();
  SetParams(params);
}

void OCIO_ACES_Transform_Op::Apply(std::shared_ptr<ImageBuffer>) {
  // auto& img = input->GetCPUData();
  throw std::runtime_error("OCIO_ACES_Transform_Op: CPU Apply not implemented yet");

//   if (!input_transform_.empty() && !output_transform_.empty()) {
//     auto input_transform = OCIO::ColorSpaceTransform::Create();
//     input_transform->setSrc(input_transform_.c_str());
//     input_transform->setDst("ACES - ACES2065-1");
//     auto                  idt = config_->getProcessor(input_transform);
//     auto                  cpu = idt->getDefaultCPUProcessor();
//     OCIO::PackedImageDesc desc_idt(img.ptr<float>(0), img.cols, img.rows, 3);
//     cpu->apply(desc_idt);

//     auto output_transform = OCIO::LookTransform::Create();
//     output_transform->setLooks("ACES 1.3 Reference Gamut Compression");
//     output_transform->setSrc("ACES - ACES2065-1");
//     output_transform->setDst(output_transform_.c_str());
//     auto                  odt     = config_->getProcessor(output_transform);
//     auto                  odt_cpu = odt->getDefaultCPUProcessor();
//     OCIO::PackedImageDesc desc_odt(img.ptr<float>(0), img.cols, img.rows, 3);
//     odt_cpu->apply(desc_odt);
//   } else if (!input_transform_.empty() && output_transform_.empty()) {
//     auto transform = OCIO::ColorSpaceTransform::Create();
//     // transform->setLooks("ACES 1.3 Reference Gamut Compression");
//     transform->setSrc(input_transform_.c_str());
//     transform->setDst("ACES - ACES2065-1");
//     auto                  idt = config_->getProcessor(transform);
//     auto                  cpu = idt->getDefaultCPUProcessor();

//     OCIO::PackedImageDesc desc(img.ptr<float>(0), img.cols, img.rows, 3);

//     cpu->apply(desc);

//   } else if (input_transform_.empty() && !output_transform_.empty() &&
//              output_transform_.ends_with("Display")) {
//     auto transform = OCIO::DisplayViewTransform::Create();
//     transform->setSrc("ACES - ACES2065-1");
//     transform->setDisplay(output_transform_.c_str());
//     transform->setView("ACES 2.0 - SDR 100 nits (Rec.709)");

//     auto                  odt = config_->getProcessor(transform);
//     auto                  cpu = odt->getDefaultCPUProcessor();

//     OCIO::PackedImageDesc desc(img.ptr<float>(0), img.cols, img.rows, 3);

//     cpu->apply(desc);
//   } else if (input_transform_.empty() && !output_transform_.empty()) {
//     auto transform = OCIO::LookTransform::Create();
//     transform->setLooks("ACES 1.3 Reference Gamut Compression");
//     transform->setSrc("ACES - ACES2065-1");
//     transform->setDst(output_transform_.c_str());
//     transform->setDirection(OCIO::TransformDirection::TRANSFORM_DIR_FORWARD);

//     auto                  csc = config_->getProcessor(transform);
//     auto                  cpu = csc->getDefaultCPUProcessor();
//     OCIO::PackedImageDesc desc(img.ptr<float>(0), img.cols, img.rows, 3);

//     cpu->apply(desc);
//   }
}

auto OCIO_ACES_Transform_Op::ApplyLMT(ImageBuffer&) -> ImageBuffer {
  throw std::runtime_error("OCIO_ACES_Transform_Op: LMT application not implemented yet");
}

void OCIO_ACES_Transform_Op::ApplyGPU(std::shared_ptr<ImageBuffer>) {
  throw std::runtime_error("OCIO_ACES_Transform_Op: GPU Apply not implemented yet");
}

auto OCIO_ACES_Transform_Op::GetParams() const -> nlohmann::json {
  nlohmann::json o;
  nlohmann::json inner;

  inner["src"]            = input_transform_;
  inner["dest"]           = output_transform_;
  inner["limit"]          = limit_;
  inner["normalize"]      = normalize_;
  inner["transform_type"] = static_cast<uint32_t>(transform_type_);

  if (lmt_path_.has_value()) {
    inner["lmt"] = lmt_path_->u8string();
  }
  o[script_name_] = inner;

  return o;
}

void OCIO_ACES_Transform_Op::SetCSTProcessors(const char* , const char* ) {
  throw std::runtime_error("OCIO_ACES_Transform_Op: CST processor setup not implemented yet");
  // auto transform = OCIO::ColorSpaceTransform::Create();
  // transform->setSrc(input);
  // transform->setDst(output);
  // auto proc     = config_->getProcessor(transform);
  // auto cpu_proc = proc->getDefaultCPUProcessor();
  // auto gpu_proc = proc->getDefaultGPUProcessor();

  // cpu_processor_ = cpu_proc;
  // gpu_processor_ = gpu_proc;

  // try {
  //   baker_ = OCIO::Baker::Create();
  //   baker_->setConfig(config_);
  //   baker_->setFormat("iridas_cube");

  //   baker_->setCubeSize(LUT3D_EDGE_SIZE);
  // } catch (OCIO::Exception& e) {
  //   std::cout << "OCIO Exception: " << e.what() << std::endl;
  //   throw std::runtime_error(
  //       std::string("OCIO_ACES_Transform_Op: Failed to set LUT size in baker: ") + e.what());
  // }
  // baker_->setInputSpace(input);
  // baker_->setTargetSpace(output);
}

void OCIO_ACES_Transform_Op::SetDisplayProcessors(const char* output) {
  (void)output;
  throw std::runtime_error("OCIO_ACES_Transform_Op: Display processor not implemented yet");
}

void OCIO_ACES_Transform_Op::SetParams(const nlohmann::json& params) {
  if (!params.contains(script_name_)) {
    throw std::invalid_argument("CST Operator: Not a valid adjustments JSON");
  }
  nlohmann::json inner = params[script_name_].get<nlohmann::json>();
  if (!inner.contains("src") || !inner.contains("dst")) {
    throw std::invalid_argument("CST Operator: Not a valid adjustments JSON");
  }
  input_transform_  = inner["src"].get<std::string>();
  output_transform_ = inner["dst"].get<std::string>();
  if (inner.contains("limit")) {
    limit_ = inner["limit"].get<bool>();
  }

  if (inner.contains("normalize")) {
    normalize_ = inner["normalize"].get<bool>();
  }

  if (inner.contains("transform_type")) {
    transform_type_ = static_cast<TransformType>(inner["transform_type"].get<uint32_t>());
  }
}

void OCIO_ACES_Transform_Op::SetGlobalParams(OperatorParams& params) const {
  switch (transform_type_) {
    case TransformType::To_WorkingSpace:
      // params.cpu_to_working_processor_ = cpu_processor_;
      // params.gpu_to_working_processor_ = gpu_processor_;
      // params.to_ws_lut_baker_          = baker_;
      params.to_ws_dirty_              = true;
      break;
    case TransformType::To_OutputSpace:
      // params.cpu_to_output_processor_ = cpu_processor_;
      // params.gpu_to_output_processor_ = gpu_processor_;
      // params.to_output_lut_baker_     = baker_;
      params.to_output_dirty_         = true;
      break;
  }
}

void OCIO_ACES_Transform_Op::EnableGlobalParams(OperatorParams& params, bool enable) {
  switch (transform_type_) {
    case TransformType::To_WorkingSpace:
      params.to_ws_enabled_ = enable;
      break;
    case TransformType::To_OutputSpace:
      params.to_output_enabled_ = enable;
      break;
  }
}
};  // namespace alcedo
