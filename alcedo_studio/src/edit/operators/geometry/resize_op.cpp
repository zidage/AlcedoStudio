//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/geometry/resize_op.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "edit/operators/op_base.hpp"
#include "image/image_buffer.hpp"
#ifdef HAVE_CUDA
#include "edit/operators/geometry/cuda_geometry_ops.hpp"
#endif
#ifdef HAVE_METAL
#include "metal/metal_utils/geometry_utils.hpp"
#endif
#ifdef HAVE_OPENCL
#include "opencl/opencl_geometry_utils.hpp"
#endif

namespace alcedo {
namespace {

constexpr float kScaleEpsilon = 1e-4f;

struct ResizePlan {
  bool     noop         = false;
  bool     use_roi      = false;
  bool     needs_resize = false;
  cv::Rect roi_rect;
  cv::Size output_size;
};

auto BuildResizePlan(int w, int h, bool enable_scale, int maximum_edge, bool enable_roi,
                     const ROI& roi) -> ResizePlan {
  ResizePlan plan;
  if (w <= 0 || h <= 0 || (!enable_scale && !enable_roi)) {
    plan.noop = true;
    return plan;
  }

  const float full_scale =
      enable_scale
          ? std::min(1.0f, static_cast<float>(maximum_edge) / static_cast<float>(std::max(w, h)))
          : 1.0f;

  if (!enable_roi) {
    if (full_scale >= (1.0f - kScaleEpsilon)) {
      plan.noop = true;
      return plan;
    }

    plan.output_size =
        cv::Size(std::max(1, static_cast<int>(std::lround(static_cast<float>(w) * full_scale))),
                 std::max(1, static_cast<int>(std::lround(static_cast<float>(h) * full_scale))));
    plan.needs_resize = true;
    return plan;
  }

  const float roi_factor_x = std::clamp(
      roi.resize_factor_x_ > 0.0f ? roi.resize_factor_x_ : roi.resize_factor_, 1e-4f, 1.0f);
  const float roi_factor_y = std::clamp(
      roi.resize_factor_y_ > 0.0f ? roi.resize_factor_y_ : roi.resize_factor_, 1e-4f, 1.0f);
  const int roi_w =
      std::clamp(static_cast<int>(std::lround(static_cast<float>(w) * roi_factor_x)), 1, w);
  const int roi_h =
      std::clamp(static_cast<int>(std::lround(static_cast<float>(h) * roi_factor_y)), 1, h);
  const int roi_x = [&]() {
    if (roi.reference_width_ > 0) {
      const float normalized_x =
          std::clamp(static_cast<float>(std::max(0, roi.x_)) /
                         static_cast<float>(std::max(1, roi.reference_width_)),
                     0.0f, 1.0f);
      return std::clamp(static_cast<int>(std::lround(normalized_x * static_cast<float>(w))), 0,
                        std::max(0, w - roi_w));
    }
    return std::clamp(roi.x_, 0, std::max(0, w - roi_w));
  }();
  const int roi_y = [&]() {
    if (roi.reference_height_ > 0) {
      const float normalized_y =
          std::clamp(static_cast<float>(std::max(0, roi.y_)) /
                         static_cast<float>(std::max(1, roi.reference_height_)),
                     0.0f, 1.0f);
      return std::clamp(static_cast<int>(std::lround(normalized_y * static_cast<float>(h))), 0,
                        std::max(0, h - roi_h));
    }
    return std::clamp(roi.y_, 0, std::max(0, h - roi_h));
  }();

  plan.use_roi  = true;
  plan.roi_rect = cv::Rect(roi_x, roi_y, roi_w, roi_h);

  const float roi_scale =
      enable_scale
          ? std::min(1.0f,
                     static_cast<float>(std::max(1, maximum_edge)) /
                         static_cast<float>(std::max(roi_w, roi_h)))
          : 1.0f;
  if (roi_scale < (1.0f - kScaleEpsilon)) {
    plan.output_size =
        cv::Size(std::max(1, static_cast<int>(std::lround(static_cast<float>(roi_w) * roi_scale))),
                 std::max(1, static_cast<int>(std::lround(static_cast<float>(roi_h) * roi_scale))));
    plan.needs_resize = true;
  } else {
    plan.output_size = cv::Size(roi_w, roi_h);
  }

  if (plan.roi_rect.x == 0 && plan.roi_rect.y == 0 && plan.roi_rect.width == w &&
      plan.roi_rect.height == h && !plan.needs_resize) {
    plan.noop = true;
  }

  return plan;
}

auto DownsampleAlgorithmToParam(ResizeDownsampleAlgorithm algorithm) -> const char* {
  switch (algorithm) {
    case ResizeDownsampleAlgorithm::Bilinear:
      return "bilinear";
    case ResizeDownsampleAlgorithm::Area:
      return "inter_area";
  }

  throw std::runtime_error("ResizeOp: unsupported downsample algorithm");
}

auto ParseDownsampleAlgorithm(const nlohmann::json& resize_params) -> ResizeDownsampleAlgorithm {
  const std::string algorithm =
      resize_params.value("downsample_algorithm", std::string("inter_area"));
  if (algorithm == "bilinear" || algorithm == "linear") {
    return ResizeDownsampleAlgorithm::Bilinear;
  }
  if (algorithm == "inter_area" || algorithm == "area") {
    return ResizeDownsampleAlgorithm::Area;
  }

  throw std::runtime_error("ResizeOp::SetParams: unsupported downsample_algorithm");
}

auto ToOpenCvInterpolation(ResizeDownsampleAlgorithm algorithm) -> int {
  switch (algorithm) {
    case ResizeDownsampleAlgorithm::Bilinear:
      return cv::INTER_LINEAR;
    case ResizeDownsampleAlgorithm::Area:
      return cv::INTER_AREA;
  }

  throw std::runtime_error("ResizeOp: unsupported downsample algorithm");
}

#ifdef HAVE_CUDA
void ResizeGpuMatWithCuda(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst, cv::Size output_size,
                          ResizeDownsampleAlgorithm downsample_algorithm) {
  if (output_size.width <= 0 || output_size.height <= 0) {
    throw std::runtime_error("ResizeOp::ApplyGPU: destination size must be positive");
  }

  if (src.cols == output_size.width && src.rows == output_size.height) {
    src.copyTo(dst);
    return;
  }

  if (src.cols <= output_size.width || src.rows <= output_size.height) {
    CUDA::ResizeLinear(src, dst, output_size);
    return;
  }

  if (downsample_algorithm == ResizeDownsampleAlgorithm::Bilinear) {
    CUDA::ResizeLinear(src, dst, output_size);
    return;
  }

  CUDA::ResizeAreaApprox(src, dst, output_size);
}
#endif

#ifdef HAVE_OPENCL
void ResizeOpenClImage(const opencl::OpenClImage& src, opencl::OpenClImage& dst,
                       cv::Size output_size, ResizeDownsampleAlgorithm downsample_algorithm) {
  if (output_size.width <= 0 || output_size.height <= 0) {
    throw std::runtime_error("ResizeOp::ApplyGPU: destination size must be positive");
  }

  if (src.Width() == output_size.width && src.Height() == output_size.height) {
    src.CopyTo(dst);
    return;
  }

  if (src.Width() <= output_size.width || src.Height() <= output_size.height) {
    OpenCL::Geometry::ResizeLinear(src, dst, output_size);
    return;
  }

  if (downsample_algorithm == ResizeDownsampleAlgorithm::Bilinear) {
    OpenCL::Geometry::ResizeLinear(src, dst, output_size);
    return;
  }

  OpenCL::Geometry::ResizeAreaApprox(src, dst, output_size);
}
#endif

}  // namespace

ResizeOp::ResizeOp(const nlohmann::json& params) { SetParams(params); }

void ResizeOp::Apply(std::shared_ptr<ImageBuffer> input) {
  auto&      img = input->GetCPUData();
  const auto plan =
      BuildResizePlan(img.cols, img.rows, enable_scale_, maximum_edge_, enable_roi_, roi_);
  if (plan.noop) {
    return;
  }

  if (plan.use_roi) {
    cv::Mat roi_img = img(plan.roi_rect).clone();
    if (plan.needs_resize) {
      cv::resize(roi_img, roi_img, plan.output_size, 0, 0,
                 ToOpenCvInterpolation(downsample_algorithm_));
    }
    img = roi_img;
    return;
  }

  cv::resize(img, img, plan.output_size, 0, 0, ToOpenCvInterpolation(downsample_algorithm_));
}

void ResizeOp::ApplyGPU(std::shared_ptr<ImageBuffer> input) {
#if !defined(HAVE_CUDA) && !defined(HAVE_METAL) && !defined(HAVE_OPENCL)
  throw std::runtime_error("ResizeOp::ApplyGPU requires HAVE_CUDA, HAVE_METAL, or HAVE_OPENCL");
#else
  if (!input->gpu_data_valid_) {
    input->SyncToGPU();
  }

#ifdef HAVE_OPENCL
  if (input->GetGPUBackend() == GpuBackendKind::OpenCL) {
    auto&      img = input->GetOpenClImage();
    const auto plan =
        BuildResizePlan(img.Width(), img.Height(), enable_scale_, maximum_edge_, enable_roi_, roi_);
    if (plan.noop) {
      return;
    }

    if (plan.use_roi) {
      opencl::OpenClImage dst;
      OpenCL::Geometry::CropResize(img, dst, plan.roi_rect, plan.output_size,
                                   downsample_algorithm_);
      img = std::move(dst);
      return;
    }

    opencl::OpenClImage dst;
    ResizeOpenClImage(img, dst, plan.output_size, downsample_algorithm_);
    img = std::move(dst);
    return;
  }
#endif

#ifdef HAVE_CUDA
  if (input->GetGPUBackend() == GpuBackendKind::CUDA) {
    auto&      img = input->GetCUDAImage();
    const auto plan =
        BuildResizePlan(img.cols, img.rows, enable_scale_, maximum_edge_, enable_roi_, roi_);
    if (plan.noop) {
      return;
    }

    if (plan.use_roi) {
      cv::cuda::GpuMat roi_src = img(plan.roi_rect);
      if (!plan.needs_resize) {
        img = roi_src.clone();
        return;
      }

      cv::cuda::GpuMat roi_dst;
      ResizeGpuMatWithCuda(roi_src, roi_dst, plan.output_size, downsample_algorithm_);
      img = std::move(roi_dst);

      return;
    }

    cv::cuda::GpuMat dst;
    ResizeGpuMatWithCuda(img, dst, plan.output_size, downsample_algorithm_);

    img = std::move(dst);
    return;
  }
#endif

#ifdef HAVE_METAL
  if (input->GetGPUBackend() == GpuBackendKind::Metal) {
    auto&      img  = input->GetMetalImage();
    const auto plan = BuildResizePlan(static_cast<int>(img.Width()), static_cast<int>(img.Height()),
                                      enable_scale_, maximum_edge_, enable_roi_, roi_);
    if (plan.noop) {
      return;
    }

    metal::MetalImage dst;
    if (plan.use_roi) {
      metal::utils::CropResizeTexture(img, dst, plan.roi_rect, plan.output_size,
                                      downsample_algorithm_);
    } else {
      metal::utils::ResizeTexture(img, dst, plan.output_size, downsample_algorithm_);
    }
    img = std::move(dst);
    return;
  }
#endif

  throw std::runtime_error("ResizeOp::ApplyGPU: active GPU backend is not supported");
#endif
}

auto ResizeOp::GetParams() const -> nlohmann::json {
  nlohmann::json params;
  nlohmann::json inner;
  inner["enable_scale"]         = enable_scale_;
  inner["maximum_edge"]         = maximum_edge_;
  inner["enable_roi"]           = enable_roi_;
  inner["downsample_algorithm"] = DownsampleAlgorithmToParam(downsample_algorithm_);
  inner["roi"]                  = {{"x", roi_.x_},
                                   {"y", roi_.y_},
                                   {"resize_factor_x", roi_.resize_factor_x_},
                                   {"resize_factor_y", roi_.resize_factor_y_},
                                   {"resize_factor", roi_.resize_factor_},
                                   {"reference_width", roi_.reference_width_},
                                   {"reference_height", roi_.reference_height_}};

  params[script_name_]          = inner;
  return params;
}

auto ResizeOp::SetParams(const nlohmann::json& params) -> void {
  if (params.contains(script_name_)) {
    auto inner = params.at(script_name_);
    if (inner.contains("enable_scale")) {
      enable_scale_ = inner.at("enable_scale").get<bool>();
    } else {
      enable_scale_ = false;
    }
    if (inner.contains("maximum_edge")) {
      maximum_edge_ = inner.at("maximum_edge").get<int>();
    } else {
      maximum_edge_ = 2048;
    }
    if (inner.contains("enable_roi")) {
      enable_roi_ = inner.at("enable_roi").get<bool>();
    } else {
      enable_roi_ = false;
    }
    downsample_algorithm_ = ParseDownsampleAlgorithm(inner);
    if (enable_roi_ && inner.contains("roi")) {
      auto roi_json          = inner.at("roi");
      roi_.x_                = roi_json.value("x", 0);
      roi_.y_                = roi_json.value("y", 0);
      roi_.resize_factor_    = roi_json.value("resize_factor", 1.0f);
      roi_.resize_factor_x_  = roi_json.value("resize_factor_x", roi_.resize_factor_);
      roi_.resize_factor_y_  = roi_json.value("resize_factor_y", roi_.resize_factor_);
      roi_.reference_width_  = roi_json.value("reference_width", 0);
      roi_.reference_height_ = roi_json.value("reference_height", 0);
    }
  } else {
    enable_scale_         = false;
    maximum_edge_         = 2048;
    enable_roi_           = false;
    downsample_algorithm_ = ResizeDownsampleAlgorithm::Area;
    roi_                  = {0, 0, 1.0f, 1.0f, 1.0f, 0, 0};
  }
}

void ResizeOp::SetGlobalParams(OperatorParams&) const {
  // throw std::runtime_error("ResizeOp does not support global parameters.");
  // DO NOTHING
}

void ResizeOp::EnableGlobalParams(OperatorParams&, bool) {
  // Still DO NOTHING
  // ResizeOp is not a streamable operator
}
};  // namespace alcedo
