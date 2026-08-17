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

#if defined(_OPENMP)
#include <omp.h>
#endif

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

void AppendUnpackOpenMpLog(DeferredCpuLog& log) {
#if defined(_OPENMP)
  std::ostringstream oss;
  oss << "RAW CPU unpack_omp_max_threads=" << omp_get_max_threads();
  log.Add(oss.str());
#else
  (void)log;
#endif
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

}  // namespace

void RawDecodeOp::SetRuntimeGpuBackend(const GpuBackendKind backend) {
  switch (backend) {
    case GpuBackendKind::None:
      params_.gpu_backend_ = RawGpuBackend::CPU;
      return;
    case GpuBackendKind::CUDA:
      params_.gpu_backend_ = RawGpuBackend::CUDA;
      return;
    case GpuBackendKind::OpenCL:
      params_.gpu_backend_ = RawGpuBackend::OpenCL;
      return;
    case GpuBackendKind::Metal:
      params_.gpu_backend_ = RawGpuBackend::Metal;
      return;
  }
  params_.gpu_backend_ = RawGpuBackend::CPU;
}

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
  const auto dng_metadata =
      dng::ExtractMetadata(std::span<const uint8_t>(buffer.data(), buffer.size()));

  switch (backend_) {
    case RawProcessBackend::ALCEDO: {
      const auto unpack_start = ProfileClock::now();
      throw_if_cancelled();
      libraw_guard::Unpack(*raw_processor);
      AppendProfileMs(deferred_log, "RAW CPU unpack", ProfileClock::now() - unpack_start);
      AppendUnpackOpenMpLog(deferred_log);
      AppendLibRawUnpackRouteLog(deferred_log, *raw_processor);
      throw_if_cancelled();

      // Prefer image-local inherent context written at import; fall back to
      // extracting directly from the open LibRaw instance.
      RawRuntimeColorContext ctx = inherent_raw_context_;
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
      inherent_raw_context_ = processor.GetRuntimeColorContext();
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
      AppendUnpackOpenMpLog(deferred_log);
      AppendLibRawUnpackRouteLog(deferred_log, *raw_processor);
      throw_if_cancelled();

      RawRuntimeColorContext ctx = inherent_raw_context_;
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
      inherent_raw_context_                           = ctx;
      inherent_raw_context_.output_in_camera_space_   = false;
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

  // The accelerator backend is deliberately NOT part of the params: it is a
  // runtime property owned by the pipeline (user backend setting), so it must
  // never be persisted into the edit state.
  // decode_res is a one-shot render parameter and must not appear in durable export.
  inner["method"]                 = RawDemosaicMethodToString(params_.demosaic_method_);
  inner["highlights_reconstruct"] = params_.highlights_reconstruct_;
  inner["use_camera_wb"]          = params_.use_camera_wb_;
  inner["user_wb"]                = params_.user_wb_;
  inner["backend"]                = (backend_ == RawProcessBackend::ALCEDO) ? "alcedo" : "libraw";

  // Persist image-local RAW color/lens metadata so reload works without InjectRawMetadata.
  if (inherent_raw_context_.valid_ || inherent_raw_context_.color_matrices_valid_ ||
      inherent_raw_context_.forward_matrices_valid_ || !inherent_raw_context_.camera_make_.empty() ||
      !inherent_raw_context_.camera_model_.empty()) {
    const auto context_json = RawColorContextToJson(inherent_raw_context_);
    for (auto it = context_json.begin(); it != context_json.end(); ++it) {
      inner[it.key()] = it.value();
    }
  }

  params["raw"] = std::move(inner);
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
  // Backend keys in stored params (gpu_backend/cuda/opencl) are ignored: the
  // accelerator backend comes only from the runtime preference pushed by the
  // pipeline executor via SetRuntimeGpuBackend.
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
  // One-shot decode resolution may still be installed via SetParams for a single Apply.
  if (inner.contains("decode_res"))
    params_.decode_res_ = static_cast<DecodeRes>(inner["decode_res"].get<int>());

  RawRuntimeColorContext loaded_context;
  if (RawColorContextFromJson(inner, loaded_context)) {
    inherent_raw_context_ = std::move(loaded_context);
  }
}

void RawDecodeOp::SetGlobalParams(OperatorParams& params) const {
  const auto& ctx = inherent_raw_context_;

  const bool inherent_changed =
      params.raw_runtime_valid_ != ctx.valid_ || params.raw_camera_make_ != ctx.camera_make_ ||
      params.raw_camera_model_ != ctx.camera_model_ ||
      params.raw_color_matrices_valid_ != ctx.color_matrices_valid_ ||
      params.raw_forward_matrices_valid_ != ctx.forward_matrices_valid_ ||
      params.raw_as_shot_neutral_valid_ != ctx.as_shot_neutral_valid_ ||
      params.raw_lens_make_ != ctx.lens_make_ || params.raw_lens_model_ != ctx.lens_model_;

  params.raw_runtime_valid_      = ctx.valid_;
  params.raw_decode_input_space_ =
      ctx.output_in_camera_space_ ? RawDecodeInputSpace::CAMERA : RawDecodeInputSpace::AP0;

  for (int i = 0; i < 3; ++i) {
    params.raw_cam_mul_[i] = ctx.cam_mul_[i];
    params.raw_pre_mul_[i] = ctx.pre_mul_[i];
  }

  for (int i = 0; i < 9; ++i) {
    params.raw_cam_xyz_[i] = ctx.cam_xyz_[i];
    params.raw_rgb_cam_[i] = ctx.rgb_cam_[i];
  }

  params.raw_camera_make_          = ctx.camera_make_;
  params.raw_camera_model_         = ctx.camera_model_;
  params.raw_color_matrices_valid_ = ctx.color_matrices_valid_;
  for (int i = 0; i < 9; ++i) {
    params.raw_color_matrix_1_[i]   = ctx.color_matrix_1_[i];
    params.raw_color_matrix_2_[i]   = ctx.color_matrix_2_[i];
    params.raw_forward_matrix_1_[i] = ctx.forward_matrix_1_[i];
    params.raw_forward_matrix_2_[i] = ctx.forward_matrix_2_[i];
  }
  params.raw_forward_matrices_valid_ = ctx.forward_matrices_valid_;
  params.raw_as_shot_neutral_valid_  = ctx.as_shot_neutral_valid_;
  for (int i = 0; i < 3; ++i) {
    params.raw_as_shot_neutral_[i] = ctx.as_shot_neutral_[i];
  }
  params.raw_calibration_illuminants_valid_ = ctx.calibration_illuminants_valid_;
  params.raw_color_matrix_1_cct_            = ctx.color_matrix_1_cct_;
  params.raw_color_matrix_2_cct_            = ctx.color_matrix_2_cct_;
  params.raw_lens_metadata_valid_           = ctx.lens_metadata_valid_;
  params.raw_lens_make_                     = ctx.lens_make_;
  params.raw_lens_model_                    = ctx.lens_model_;
  params.raw_lens_focal_mm_                 = ctx.focal_length_mm_;
  params.raw_lens_aperture_f_               = ctx.aperture_f_number_;
  params.raw_lens_focus_distance_m_         = ctx.focus_distance_m_;
  params.raw_lens_focal_35mm_               = ctx.focal_35mm_mm_;
  params.raw_lens_crop_factor_hint_         = ctx.crop_factor_hint_;
  params.raw_dng_warp_rectilinear_present_  = ctx.dng_warp_rectilinear_present_;
  params.raw_dng_warp_rectilinear_applied_  = ctx.dng_warp_rectilinear_applied_;

  if (inherent_changed) {
    params.lens_calib_runtime_dirty_ = true;
    params.color_temp_runtime_dirty_ = true;
  }
}

void RawDecodeOp::EnableGlobalParams(OperatorParams&, bool) {
  // Still DO NOTHING
  // RawDecodeOp is not a streamable operator
}
};  // namespace alcedo
