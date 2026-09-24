//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/cst/lmt_op.hpp"

namespace alcedo {
namespace {

auto PathToUtf8(const std::filesystem::path& path) -> std::string {
  const auto utf8 = path.generic_u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

auto Utf8OrNativeToPath(const std::string& raw_path) -> std::filesystem::path {
  if (raw_path.empty()) {
    return {};
  }
  try {
    const auto* begin = reinterpret_cast<const char8_t*>(raw_path.data());
    return std::filesystem::path(std::u8string(begin, begin + raw_path.size()));
  } catch (...) {
    return std::filesystem::path(raw_path);
  }
}

}  // namespace

OCIO_LMT_Transform_Op::OCIO_LMT_Transform_Op(std::filesystem::path& lmt_path)
    : lmt_path_(lmt_path) {
  // config_ = ocio_config::LoadBundledConfig();
}

OCIO_LMT_Transform_Op::OCIO_LMT_Transform_Op(const nlohmann::json& params) {
  // config_ = ocio_config::LoadBundledConfig();
  SetParams(params);
}

void OCIO_LMT_Transform_Op::Apply(std::shared_ptr<ImageBuffer> input) {
  throw std::runtime_error("OCIO_LMT_Transform_Op: CPU Apply not implemented yet");
  // if (lmt_path_.empty()) {
  //   return;
  // }
  // auto& img           = input->GetCPUData();

  // auto  lmt_transform = OCIO::FileTransform::Create();
  // const std::string path_utf8 = PathToUtf8(lmt_path_);
  // lmt_transform->setSrc(path_utf8.c_str());
  // lmt_transform->setInterpolation(OCIO::INTERP_BEST);
  // lmt_transform->setDirection(OCIO::TRANSFORM_DIR_FORWARD);

  // auto lmt_processor = config_->getProcessor(lmt_transform);
  // auto cpu           = lmt_processor->getDefaultCPUProcessor();

  // cv::parallel_for_(cv::Range(0, img.rows), [&](const cv::Range& range) {
  //   for (int y = range.start; y < range.end; ++y) {
  //     cv::Vec3f* row = img.ptr<cv::Vec3f>(y);
  //     for (int x = 0; x < img.cols; ++x) {
  //       cpu->applyRGB(&row[x][0]);
  //     }
  //   }
  // });
}

void OCIO_LMT_Transform_Op::ApplyGPU(std::shared_ptr<ImageBuffer>) {
  throw std::runtime_error("OCIO_LMT_Transform_Op: GPU Apply not implemented yet");
}

auto OCIO_LMT_Transform_Op::GetParams() const -> nlohmann::json {
  nlohmann::json o;
  o[script_name_] = PathToUtf8(lmt_path_);

  return o;
}

void OCIO_LMT_Transform_Op::SetParams(const nlohmann::json& params) {
  if (!params.contains(script_name_)) {
    lmt_path_ = std::filesystem::path();
    return;
  }
  const std::string raw_path = params[script_name_].get<std::string>();
  if (raw_path.empty()) {
    lmt_path_ = std::filesystem::path();
    return;
  }
  lmt_path_ = Utf8OrNativeToPath(raw_path);

  // auto lmt_transform = OCIO::FileTransform::Create();
  // auto path_str      = lmt_path_.wstring();
  // lmt_transform->setSrc(conv::ToBytes(path_str).c_str());
  // lmt_transform->setInterpolation(OCIO::INTERP_BEST);
  // lmt_transform->setDirection(OCIO::TRANSFORM_DIR_FORWARD);

  // auto lmt_processor = config_->getProcessor(lmt_transform);
  // auto cpu           = lmt_processor->getDefaultCPUProcessor();
  // auto gpu           = lmt_processor->getDefaultGPUProcessor();
  // cpu_processor_      = cpu;
  // gpu_processor_      = gpu;
}

void OCIO_LMT_Transform_Op::SetGlobalParams(OperatorParams& params) const {
  // params.cpu_lmt_processor_ = cpu_processor_;
  // params.gpu_lmt_processor_ = gpu_processor_;

  params.lmt_lut_path_  = lmt_path_;
  params.lmt_enabled_   = !lmt_path_.empty();
  // Only mark dirty when enabled; otherwise GPU upload would attempt to parse an empty path.
  params.to_lmt_dirty_  = params.lmt_enabled_;
}

void OCIO_LMT_Transform_Op::EnableGlobalParams(OperatorParams& params, bool enable) {
  params.lmt_enabled_  = enable;
  params.to_lmt_dirty_ = enable;
}
};  // namespace alcedo
