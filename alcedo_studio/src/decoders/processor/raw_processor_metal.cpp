//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/raw_processor.hpp"

#ifdef HAVE_METAL

#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "decoders/processor/nn/demosaicnet_preprocess_common.hpp"
#include "decoders/processor/nn/demosaicnet_specs.hpp"
#include "decoders/processor/operators/gpu/metal_cvt_ref_space.hpp"
#include "decoders/processor/operators/gpu/metal_debayer_rcd.hpp"
#include "decoders/processor/operators/gpu/metal_demosaicnet.hpp"
#include "decoders/processor/operators/gpu/metal_highlight_reconstruct.hpp"
#include "decoders/processor/operators/gpu/metal_to_linear_ref.hpp"
#include "decoders/processor/operators/gpu/metal_xtrans_interpolate.hpp"
#include "decoders/processor/raw_processor_internal.hpp"
#include "metal/metal_utils/geometry_utils.hpp"
#include "metal/metal_utils/metal_convert_utils.hpp"

namespace alcedo {
namespace {

using ProfileClock = std::chrono::steady_clock;

struct DeferredMetalLog {
  std::vector<std::string> entries;

  void Add(std::string entry) { entries.push_back(std::move(entry)); }

  void Flush() const {
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

void PrintProfileMs(DeferredMetalLog& log, const char* label, const ProfileClock::duration elapsed) {
  std::ostringstream oss;
  oss << label << '=' << std::chrono::duration<double, std::milli>(elapsed).count() << " ms";
  log.Add(oss.str());
}

void LogProfileStep(DeferredMetalLog& log, const char* label, const ProfileClock::time_point start) {
  PrintProfileMs(log, label, ProfileClock::now() - start);
}

void ApplyMetalGeometricCorrections(metal::MetalImage& gpu_img, const int flip) {
  switch (flip) {
    case 3:
      metal::utils::Rotate180(gpu_img);
      break;
    case 5:
      metal::utils::Rotate90CCW(gpu_img);
      break;
    case 6:
      metal::utils::Rotate90CW(gpu_img);
      break;
    default:
      break;
  }
}

void FinishMetalCameraRgb(metal::MetalImage& gpu_img, const float* cam_mul,
                          const std::optional<dng::WarpRectilinear>& dng_warp,
                          RawRuntimeColorContext& runtime_color_context, const int flip,
                          DeferredMetalLog& log) {
  const auto stage_wb_start = ProfileClock::now();
  metal::ApplyInverseCamMul(gpu_img, cam_mul);
  LogProfileStep(log, "RAW Metal inverse cam mul", stage_wb_start);
  if (dng_warp.has_value()) {
    const auto        stage_warp_start = ProfileClock::now();
    metal::MetalImage warped;
    metal::utils::WarpRectilinearTexture(gpu_img, warped, *dng_warp);
    gpu_img                                             = std::move(warped);
    runtime_color_context.dng_warp_rectilinear_applied_ = true;
    LogProfileStep(log, "RAW Metal dng warp", stage_warp_start);
  }
  runtime_color_context.output_in_camera_space_ = true;
  const auto stage_geo_start                    = ProfileClock::now();
  ApplyMetalGeometricCorrections(gpu_img, flip);
  LogProfileStep(log, "RAW Metal geometric corrections", stage_geo_start);
}

void RunMetalLegacyDemosaic(metal::MetalImage& gpu_img, const RawCfaPattern& cfa_pattern,
                            const RawParams& params, LibRaw& raw_processor,
                            const libraw_image_sizes_t& sizes, const ushort default_crop[4],
                            DeferredMetalLog& log) {
  if (cfa_pattern.kind == RawCfaKind::Bayer2x2 && params.highlights_reconstruct_) {
    const auto stage_rcd_start = ProfileClock::now();
    metal::Bayer2x2ToRGB_RCD(gpu_img, cfa_pattern.bayer_pattern);
    LogProfileStep(log, "RAW Metal RCD demosaic", stage_rcd_start);
    const cv::Rect crop_rect = detail::BuildRcdDecodeCropRect(
        sizes, default_crop, cv::Size(gpu_img.Width(), gpu_img.Height()), params.decode_res_);
    if (!detail::IsFullImageRect(crop_rect, cv::Size(gpu_img.Width(), gpu_img.Height()))) {
      const auto        stage_crop_start = ProfileClock::now();
      metal::MetalImage cropped;
      gpu_img.CropTo(cropped, crop_rect);
      gpu_img = std::move(cropped);
      LogProfileStep(log, "RAW Metal crop", stage_crop_start);
    }
    const auto stage_hlr_start = ProfileClock::now();
    metal::HighlightReconstruct(gpu_img, raw_processor);
    LogProfileStep(log, "RAW Metal highlight reconstruct", stage_hlr_start);
    return;
  }

  // ToLinearRef already writes a [0, 1] CFA. A second full-frame clamp here only
  // repeats the old texture↔buffer blit and adds ~20 ms on a 45 MP Nikon HE file.
  if (cfa_pattern.kind == RawCfaKind::XTrans6x6) {
    const auto stage_xtrans_start = ProfileClock::now();
    const int  passes             = params.decode_res_ == DecodeRes::FULL ? 3 : 1;
    metal::XTransToRGB_Ref(gpu_img, cfa_pattern.xtrans_pattern, passes);
    LogProfileStep(log, "RAW Metal X-Trans interpolate", stage_xtrans_start);
  } else {
    const auto stage_rcd_start = ProfileClock::now();
    metal::Bayer2x2ToRGB_RCD(gpu_img, cfa_pattern.bayer_pattern);
    LogProfileStep(log, "RAW Metal RCD demosaic", stage_rcd_start);
  }
  const cv::Size crop_size(gpu_img.Width(), gpu_img.Height());
  const cv::Rect crop_rect =
      cfa_pattern.kind == RawCfaKind::Bayer2x2
          ? detail::BuildRcdDecodeCropRect(sizes, default_crop, crop_size, params.decode_res_)
          : detail::BuildDecodeCropRect(sizes, default_crop, crop_size, params.decode_res_);
  if (!detail::IsFullImageRect(crop_rect, cv::Size(gpu_img.Width(), gpu_img.Height()))) {
    const auto        stage_crop_start = ProfileClock::now();
    metal::MetalImage cropped;
    gpu_img.CropTo(cropped, crop_rect);
    gpu_img = std::move(cropped);
    LogProfileStep(log, "RAW Metal crop", stage_crop_start);
  }
}

}  // namespace

auto RawProcessor::ProcessDirectRgbMetal() -> ImageBuffer {
  process_buffer_.SyncToGPU();
  process_buffer_.ReleaseCPUData();
  auto& gpu_img = process_buffer_.GetMetalImage();
  ApplyMetalGeometricCorrections(gpu_img, raw_data_.sizes.flip);
  return {std::move(process_buffer_)};
}

auto RawProcessor::ProcessMetal() -> ImageBuffer {
  if (input_kind_ == RawInputKind::DebayeredRgb) {
    return ProcessDirectRgbMetal();
  }

  DeferredMetalLog deferred_log;
  const auto       full_frame_start = ProfileClock::now();

  const auto stage_decode_res_start = ProfileClock::now();
  SetDecodeRes(gpu_input_downsample_passes_);
  LogProfileStep(deferred_log, "RAW Metal setup decode-res", stage_decode_res_start);

  const auto stage_upload_start = ProfileClock::now();
  process_buffer_.SyncToGPU();
  process_buffer_.ReleaseCPUData();
  LogProfileStep(deferred_log, "RAW Metal sync/upload", stage_upload_start);

  auto& gpu_img = process_buffer_.GetMetalImage();

  const auto stage_linear_start = ProfileClock::now();
  metal::ToLinearRef(gpu_img, raw_processor_, cfa_pattern_);
  LogProfileStep(deferred_log, "RAW Metal to-linear", stage_linear_start);

  const RawDemosaicMethod demosaic_method =
      detail::ResolveRawDemosaicMethod(params_, cfa_pattern_.kind);

  if (demosaic_method == RawDemosaicMethod::NeuralEngine) {
    // Match CUDA product sandwich: HLR-off clamps linear CFA before Neural preprocess.
    // Do not catch Neural exceptions to run Legacy or another backend.
    if (!params_.highlights_reconstruct_) {
      const auto stage_clamp_start = ProfileClock::now();
      metal::utils::ClampTexture(gpu_img);
      LogProfileStep(deferred_log, "RAW Metal clamp", stage_clamp_start);
    }

    const cv::Size original_cfa_size(static_cast<int>(gpu_img.Width()),
                                     static_cast<int>(gpu_img.Height()));
    const int min_spatial = cfa_pattern_.kind == RawCfaKind::XTrans6x6
                                ? DemosaicNetXTransSpec::kMinSpatial
                                : DemosaicNetBayerSpec::kMinSpatial;

    std::string geo_error;
    const auto  geo = ComputeNeuralAlignedGeometry(cfa_pattern_, original_cfa_size.width,
                                                   original_cfa_size.height, min_spatial, &geo_error);
    if (!geo.has_value()) {
      const char* variant = cfa_pattern_.kind == RawCfaKind::XTrans6x6
                                ? DemosaicNetXTransSpec::kArchitecture
                                : DemosaicNetBayerSpec::kArchitecture;
      throw std::runtime_error(std::string("Metal Neural Engine failed (stage=prepare, variant=") +
                               variant + "): " + geo_error);
    }

    const detail::NeuralOutputGeometry geometry = detail::MakeStudentTiledNeuralOutputGeometry(
        geo->shift_sx, geo->shift_sy, cv::Size(geo->aligned_width, geo->aligned_height));
    const cv::Rect product_crop = detail::BuildNeuralEngineDecodeCropRect(
        raw_data_.sizes, default_crop_, original_cfa_size, params_.decode_res_, geometry);

    const auto stage_neural_start = ProfileClock::now();
    // Crop-sized result is created inside the entry; process_buffer keeps the linear CFA
    // until Neural succeeds, then the monochrome texture is replaced by RGBA.
    metal::MetalImage neural_rgba;
    (void)metal::DemosaicWithNeuralEngine(gpu_img, cfa_pattern_, geo->shift_sx, geo->shift_sy,
                                          geo->aligned_width, geo->aligned_height, product_crop,
                                          neural_rgba);
    LogProfileStep(deferred_log, "RAW Metal Neural Engine demosaic", stage_neural_start);

    gpu_img = std::move(neural_rgba);

    if (params_.highlights_reconstruct_) {
      const auto stage_hlr_start = ProfileClock::now();
      metal::HighlightReconstruct(gpu_img, raw_processor_);
      LogProfileStep(deferred_log, "RAW Metal highlight reconstruct", stage_hlr_start);
    }

    FinishMetalCameraRgb(gpu_img, raw_data_.color.cam_mul, dng_warp_rectilinear_,
                         runtime_color_context_, raw_data_.sizes.flip, deferred_log);
    PrintProfileMs(deferred_log, "RAW Metal FullFrame", ProfileClock::now() - full_frame_start);
    deferred_log.Flush();
    return {std::move(process_buffer_)};
  }

  RunMetalLegacyDemosaic(gpu_img, cfa_pattern_, params_, raw_processor_, raw_data_.sizes,
                         default_crop_, deferred_log);
  FinishMetalCameraRgb(gpu_img, raw_data_.color.cam_mul, dng_warp_rectilinear_,
                       runtime_color_context_, raw_data_.sizes.flip, deferred_log);
  PrintProfileMs(deferred_log, "RAW Metal FullFrame", ProfileClock::now() - full_frame_start);
  deferred_log.Flush();
  return {std::move(process_buffer_)};
}

}  // namespace alcedo

#endif
