//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/raw/raw_decode_op.hpp"

#include <libraw/libraw_const.h>
#include <opencv2/core/hal/interface.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <opencv2/imgproc.hpp>
#include <sstream>
#include <vector>

#include "decoders/dng_default_crop.hpp"
#include "decoders/libraw_unpack_guard.hpp"
#include "decoders/processor/raw_processor.hpp"
#include "image/image_buffer.hpp"
#include "image/metadata_extractor.hpp"

namespace alcedo {
namespace {
using ProfileClock = std::chrono::steady_clock;

struct DeferredCpuLog {
  std::vector<std::string> entries;

  void                     Add(std::string entry) { entries.push_back(std::move(entry)); }

  void                     Flush() const {
    if (entries.empty()) {
      return;
    }

    std::cout << "[LOG] ";
    for (size_t i = 0; i < entries.size(); ++i) {
      if (i != 0) {
        std::cout << " | ";
      }
      std::cout << entries[i];
    }
    std::cout << '\n';
  }
};

void AppendProfileMs(DeferredCpuLog& log, const char* label, const ProfileClock::duration elapsed) {
  std::ostringstream oss;
  oss << label << '=' << std::chrono::duration<double, std::milli>(elapsed).count() << " ms";
  log.Add(oss.str());
}

void AppendLibRawUnpackRouteLog(DeferredCpuLog& log, LibRaw& raw_processor) {
  if (const char* decoder_name = raw_processor.unpack_function_name();
      decoder_name != nullptr && decoder_name[0] != '\0') {
    log.Add(std::string("RAW CPU decoder=") + decoder_name);
  }

  const auto warnings = raw_processor.imgdata.process_warnings;
  if ((warnings & LIBRAW_WARN_RAWSPEED3_PROCESSED) != 0) {
    log.Add("RAW CPU unpack_route=rawspeed3");
    return;
  }
  if ((warnings & LIBRAW_WARN_RAWSPEED_PROCESSED) != 0) {
    log.Add("RAW CPU unpack_route=rawspeed");
    return;
  }
  if ((warnings & LIBRAW_WARN_DNGSDK_PROCESSED) != 0) {
    log.Add("RAW CPU unpack_route=dngsdk");
    return;
  }

  if ((warnings & LIBRAW_WARN_RAWSPEED3_UNSUPPORTED) != 0) {
    log.Add("RAW CPU rawspeed3=unsupported");
  }
  if ((warnings & LIBRAW_WARN_RAWSPEED_UNSUPPORTED) != 0) {
    log.Add("RAW CPU rawspeed=unsupported");
  }

  log.Add("RAW CPU unpack_route=libraw");
}

auto RawGpuBackendToString(RawGpuBackend backend) -> const char* {
  switch (backend) {
    case RawGpuBackend::CUDA:
      return "cuda";
    case RawGpuBackend::Metal:
      return "metal";
    case RawGpuBackend::OpenCL:
      return "opencl";
    case RawGpuBackend::GPU:
      return "gpu";
    case RawGpuBackend::CPU:
    default:
      return "cpu";
  }
}

}  // namespace

RawDecodeOp::RawDecodeOp(const nlohmann::json& params) { SetParams(params); }

void RawDecodeOp::Apply(std::shared_ptr<ImageBuffer> input) {
  const auto throw_if_cancelled = [this]() {
    if (cancel_requested_ && cancel_requested_()) {
      throw std::runtime_error("RawDecodeOp: cancelled");
    }
  };

  throw_if_cancelled();

  const auto              total_start = ProfileClock::now();
  DeferredCpuLog          deferred_log;
  auto&                   buffer        = input->GetBuffer();

  const auto              open_start    = ProfileClock::now();
  std::unique_ptr<LibRaw> raw_processor = std::make_unique<LibRaw>();
  int                     ret = raw_processor->open_buffer((void*)buffer.data(), buffer.size());
  AppendProfileMs(deferred_log, "RAW CPU open_buffer", ProfileClock::now() - open_start);
  throw_if_cancelled();
  if (ret != LIBRAW_SUCCESS) {
    throw std::runtime_error("RawDecodeOp: Unable to read raw file using LibRAW");
  }

  raw_processor->imgdata.params.output_bps      = 16;
  raw_processor->imgdata.rawparams.use_rawspeed = 1;
  ImageBuffer output;
  latest_runtime_context_ = {};
  const auto dng_metadata =
      dng::ExtractMetadata(std::span<const uint8_t>(buffer.data(), buffer.size()));

  switch (backend_) {
    case RawProcessBackend::ALCEDO: {
      const auto unpack_start = ProfileClock::now();
      throw_if_cancelled();
      libraw_guard::Unpack(*raw_processor);
      AppendProfileMs(deferred_log, "RAW CPU unpack", ProfileClock::now() - unpack_start);
      AppendLibRawUnpackRouteLog(deferred_log, *raw_processor);
      throw_if_cancelled();

      // Use pre-populated context injected before rendering; fall back to
      // extracting directly from the open LibRaw instance.
      RawRuntimeColorContext ctx = pre_populated_ctx_;
      if (!ctx.valid_) {
        MetadataExtractor::PopulateRuntimeContextFromOpenLibRaw(*raw_processor, ctx);
      }
      ctx.dng_warp_rectilinear_present_ = dng_metadata.warp_rectilinear.has_value();

      RawProcessor processor{
          params_, raw_processor->imgdata.rawdata,   *raw_processor,
          ctx,     dng_metadata.default_crop.data(), dng_metadata.warp_rectilinear};

      const auto process_start = ProfileClock::now();
      throw_if_cancelled();
      output = processor.Process();
      AppendProfileMs(deferred_log, "RAW CPU processor.Process",
                      ProfileClock::now() - process_start);
      throw_if_cancelled();
      latest_runtime_context_ = processor.GetRuntimeColorContext();
      raw_processor->recycle();
      break;
    }
    case RawProcessBackend::LIBRAW: {
      raw_processor->imgdata.params.output_color   = 1;
      raw_processor->imgdata.params.gamm[0]        = 1.0;  // Linear gamma
      raw_processor->imgdata.params.gamm[1]        = 1.0;
      raw_processor->imgdata.params.no_auto_bright = 0;  // Disable auto brightness
      raw_processor->imgdata.params.use_camera_wb  = 1;  // Discarded if user_wb is set for now
      raw_processor->imgdata.rawparams.use_dngsdk  = 1;

      const auto unpack_start                      = ProfileClock::now();
      throw_if_cancelled();
      libraw_guard::Unpack(*raw_processor);
      AppendProfileMs(deferred_log, "RAW CPU unpack", ProfileClock::now() - unpack_start);
      AppendLibRawUnpackRouteLog(deferred_log, *raw_processor);
      throw_if_cancelled();

      // Use pre-populated context or extract from LibRaw.
      RawRuntimeColorContext ctx = pre_populated_ctx_;
      if (!ctx.valid_) {
        MetadataExtractor::PopulateRuntimeContextFromOpenLibRaw(*raw_processor, ctx);
      }

      const auto process_start = ProfileClock::now();
      throw_if_cancelled();
      raw_processor->dcraw_process();
      libraw_processed_image_t* img = raw_processor->dcraw_make_mem_image(&ret);
      AppendProfileMs(deferred_log, "RAW CPU dcraw_process", ProfileClock::now() - process_start);
      throw_if_cancelled();
      if (ret != LIBRAW_SUCCESS) {
        throw std::runtime_error("RawDecodeOp: Unable to process raw file using LibRAW");
      }
      if (img->type != LIBRAW_IMAGE_BITMAP) {
        throw std::runtime_error("RawDecodeOp: Unsupported image type from LibRAW");
      }
      if (img->colors != 3) {
        throw std::runtime_error("RawDecodeOp: Only support 3-channel image from LibRAW");
      }
      cv::Mat result_view(img->height, img->width, CV_16UC3, img->data);
      cv::Mat result_rgb;
      result_view.convertTo(result_rgb, CV_32FC3, 1.0 / 65535.0);

      cv::Mat result_rgba;
      cv::cvtColor(result_rgb, result_rgba, cv::COLOR_RGB2RGBA);

      output                                          = ImageBuffer(std::move(result_rgba));
      latest_runtime_context_                         = ctx;
      latest_runtime_context_.output_in_camera_space_ = false;
      raw_processor->dcraw_clear_mem(img);
      raw_processor->recycle();
      break;
    }
  }
  AppendProfileMs(deferred_log, "RAW CPU total", ProfileClock::now() - total_start);
  deferred_log.Flush();
  *input = std::move(output);
}

void RawDecodeOp::ApplyGPU(std::shared_ptr<ImageBuffer> input) {
  // GPU implementation not available yet.
  Apply(input);
}

auto RawDecodeOp::GetParams() const -> nlohmann::json {
  nlohmann::json params;
  nlohmann::json inner;

  inner["gpu_backend"] = RawGpuBackendToString(params_.gpu_backend_);
  inner["method"]      = RawDemosaicMethodToString(params_.demosaic_method_);
  inner["cuda"]        = false;
#ifdef HAVE_CUDA
  inner["cuda"] =
      (params_.gpu_backend_ == RawGpuBackend::GPU || params_.gpu_backend_ == RawGpuBackend::CUDA);
#endif
  inner["opencl"] = false;
#ifdef HAVE_OPENCL
  inner["opencl"] =
      (params_.gpu_backend_ == RawGpuBackend::GPU || params_.gpu_backend_ == RawGpuBackend::OpenCL);
#endif
  inner["highlights_reconstruct"] = params_.highlights_reconstruct_;
  inner["use_camera_wb"]          = params_.use_camera_wb_;
  inner["user_wb"]                = params_.user_wb_;
  inner["backend"]                = (backend_ == RawProcessBackend::ALCEDO) ? "alcedo" : "libraw";
  inner["decode_res"]             = static_cast<int>(params_.decode_res_);

  params["raw"]                   = inner;
  return params;
}

void RawDecodeOp::SetParams(const nlohmann::json& params) {
  if (!params.is_object()) {
    throw std::runtime_error("RawDecodeOp: Params should be a json object");
  }

  nlohmann::json inner;
  if (params.contains("raw")) {
    inner = params["raw"];
  } else {
    return;
  }
  if (inner.contains("gpu_backend") && inner["gpu_backend"].is_string()) {
    const std::string backend = inner["gpu_backend"].get<std::string>();
    if (backend == "cpu") {
      params_.gpu_backend_ = RawGpuBackend::CPU;
    } else if (backend == "gpu" || backend == "auto") {
      params_.gpu_backend_ = RawGpuBackend::GPU;
    } else if (backend == "cuda") {
      params_.gpu_backend_ = RawGpuBackend::CUDA;
    } else if (backend == "metal") {
      params_.gpu_backend_ = RawGpuBackend::Metal;
    } else if (backend == "opencl") {
      params_.gpu_backend_ = RawGpuBackend::OpenCL;
    } else {
      throw std::runtime_error("RawDecodeOp: Unknown gpu_backend " + backend);
    }
  } else if (inner.contains("cuda")) {
    params_.gpu_backend_ = inner["cuda"].get<bool>() ? RawGpuBackend::CUDA : RawGpuBackend::CPU;
  } else if (inner.contains("opencl")) {
    params_.gpu_backend_ = inner["opencl"].get<bool>() ? RawGpuBackend::OpenCL : RawGpuBackend::CPU;
  }
  if (inner.contains("highlights_reconstruct"))
    params_.highlights_reconstruct_ = inner["highlights_reconstruct"].get<bool>();
  if (inner.contains("method")) {
    if (!inner["method"].is_string()) {
      throw std::runtime_error("RawDecodeOp: method should be a string");
    }
    params_.demosaic_method_ = RawDemosaicMethodFromString(inner["method"].get<std::string>());
  }
  if (inner.contains("use_camera_wb")) params_.use_camera_wb_ = inner["use_camera_wb"].get<bool>();
  if (inner.contains("user_wb")) params_.user_wb_ = inner["user_wb"].get<uint32_t>();
  if (inner.contains("backend")) {
    std::string backend = inner["backend"].get<std::string>();
    if (backend == "alcedo")
      backend_ = RawProcessBackend::ALCEDO;
    else if (backend == "libraw")
      backend_ = RawProcessBackend::LIBRAW;
    else
      throw std::runtime_error("RawDecodeOp: Unknown backend " + backend);
  }
  if (inner.contains("decode_res"))
    params_.decode_res_ = static_cast<DecodeRes>(inner["decode_res"].get<int>());
}

void RawDecodeOp::SetGlobalParams(OperatorParams& params) const {
  params.raw_runtime_valid_      = latest_runtime_context_.valid_;
  params.raw_decode_input_space_ = latest_runtime_context_.output_in_camera_space_
                                       ? RawDecodeInputSpace::CAMERA
                                       : RawDecodeInputSpace::AP0;

  for (int i = 0; i < 3; ++i) {
    params.raw_cam_mul_[i] = latest_runtime_context_.cam_mul_[i];
    params.raw_pre_mul_[i] = latest_runtime_context_.pre_mul_[i];
  }

  for (int i = 0; i < 9; ++i) {
    params.raw_cam_xyz_[i] = latest_runtime_context_.cam_xyz_[i];
    params.raw_rgb_cam_[i] = latest_runtime_context_.rgb_cam_[i];
  }

  params.raw_camera_make_          = latest_runtime_context_.camera_make_;
  params.raw_camera_model_         = latest_runtime_context_.camera_model_;
  params.raw_color_matrices_valid_ = latest_runtime_context_.color_matrices_valid_;
  for (int i = 0; i < 9; ++i) {
    params.raw_color_matrix_1_[i]   = latest_runtime_context_.color_matrix_1_[i];
    params.raw_color_matrix_2_[i]   = latest_runtime_context_.color_matrix_2_[i];
    params.raw_forward_matrix_1_[i] = latest_runtime_context_.forward_matrix_1_[i];
    params.raw_forward_matrix_2_[i] = latest_runtime_context_.forward_matrix_2_[i];
  }
  params.raw_forward_matrices_valid_ = latest_runtime_context_.forward_matrices_valid_;
  params.raw_as_shot_neutral_valid_  = latest_runtime_context_.as_shot_neutral_valid_;
  for (int i = 0; i < 3; ++i) {
    params.raw_as_shot_neutral_[i] = latest_runtime_context_.as_shot_neutral_[i];
  }
  params.raw_calibration_illuminants_valid_ =
      latest_runtime_context_.calibration_illuminants_valid_;
  params.raw_color_matrix_1_cct_           = latest_runtime_context_.color_matrix_1_cct_;
  params.raw_color_matrix_2_cct_           = latest_runtime_context_.color_matrix_2_cct_;
  params.raw_lens_metadata_valid_          = latest_runtime_context_.lens_metadata_valid_;
  params.raw_lens_make_                    = latest_runtime_context_.lens_make_;
  params.raw_lens_model_                   = latest_runtime_context_.lens_model_;
  params.raw_lens_focal_mm_                = latest_runtime_context_.focal_length_mm_;
  params.raw_lens_aperture_f_              = latest_runtime_context_.aperture_f_number_;
  params.raw_lens_focus_distance_m_        = latest_runtime_context_.focus_distance_m_;
  params.raw_lens_focal_35mm_              = latest_runtime_context_.focal_35mm_mm_;
  params.raw_lens_crop_factor_hint_        = latest_runtime_context_.crop_factor_hint_;
  params.raw_dng_warp_rectilinear_present_ = latest_runtime_context_.dng_warp_rectilinear_present_;
  params.raw_dng_warp_rectilinear_applied_ = latest_runtime_context_.dng_warp_rectilinear_applied_;

  params.lens_calib_runtime_dirty_         = true;
  params.color_temp_runtime_dirty_         = true;
}

void RawDecodeOp::EnableGlobalParams(OperatorParams&, bool) {
  // Still DO NOTHING
  // RawDecodeOp is not a streamable operator
}
};  // namespace alcedo
