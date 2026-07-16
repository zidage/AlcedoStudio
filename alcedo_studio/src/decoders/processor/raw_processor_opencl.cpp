//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/raw_processor.hpp"

#ifdef HAVE_OPENCL

#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "decoders/processor/operators/gpu/opencl_cvt_ref_space.hpp"
#include "decoders/processor/operators/gpu/opencl_debayer_rcd.hpp"
#include "decoders/processor/operators/gpu/opencl_demosaicnet.hpp"
#include "decoders/processor/operators/gpu/opencl_highlight_reconstruct.hpp"
#include "decoders/processor/operators/gpu/opencl_to_linear_ref.hpp"
#include "decoders/processor/operators/gpu/opencl_xtrans_interpolate.hpp"
#include "decoders/processor/raw_processor_internal.hpp"
#include "opencl/opencl_geometry_utils.hpp"

namespace alcedo {

namespace {

using ProfileClock = std::chrono::steady_clock;

struct DeferredOpenClLog {
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

void PrintProfileMs(DeferredOpenClLog& log, const char* label,
                    const ProfileClock::duration elapsed) {
  std::ostringstream oss;
  oss << label << '=' << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
      << " ms";
  log.Add(oss.str());
}

void LogProfileStep(DeferredOpenClLog& log, const char* label,
                    const ProfileClock::time_point start) {
  PrintProfileMs(log, label, ProfileClock::now() - start);
}

void ApplyOpenClGeometricCorrections(opencl::OpenClImage& gpu_img, const int flip) {
  switch (flip) {
    case 3:
      OpenCL::Geometry::Rotate180(gpu_img);
      break;
    case 5:
      OpenCL::Geometry::Rotate90CCW(gpu_img);
      break;
    case 6:
      OpenCL::Geometry::Rotate90CW(gpu_img);
      break;
    default:
      break;
  }
}

void CropOpenClImage(opencl::OpenClImage& gpu_img, const cv::Rect& crop_rect) {
  if (detail::IsFullImageRect(crop_rect, cv::Size(gpu_img.Width(), gpu_img.Height()))) {
    return;
  }
  opencl::OpenClImage cropped;
  OpenCL::Geometry::CropResize(gpu_img, cropped, crop_rect, crop_rect.size());
  gpu_img = std::move(cropped);
}

// OpenCL Legacy demosaic from preserved linear CFA (CV_32FC1). Used for the normal
// Legacy path and for same-backend Neural soft-fail. Never switches to CUDA.
void RunOpenClLegacyDemosaic(opencl::OpenClImage& gpu_img, const RawCfaPattern& cfa_pattern,
                             const RawParams& params, LibRaw& raw_processor,
                             const libraw_image_sizes_t& sizes, const ushort default_crop[4],
                             DeferredOpenClLog& deferred_log) {
  if (cfa_pattern.kind == RawCfaKind::Bayer2x2 && params.highlights_reconstruct_) {
    const auto stage_debayer_start = ProfileClock::now();
    OpenCL::Bayer2x2ToRGB_RCD(gpu_img, cfa_pattern.bayer_pattern);
    LogProfileStep(deferred_log, "RAW OpenCL debayer", stage_debayer_start);

    const auto     stage_crop_start = ProfileClock::now();
    const cv::Rect crop_rect        = detail::BuildRcdDecodeCropRect(
        sizes, default_crop, cv::Size(gpu_img.Width(), gpu_img.Height()), params.decode_res_);
    CropOpenClImage(gpu_img, crop_rect);
    LogProfileStep(deferred_log, "RAW OpenCL crop", stage_crop_start);

    const auto stage_highlight_start = ProfileClock::now();
    OpenCL::HighlightReconstruct(gpu_img, raw_processor);
    LogProfileStep(deferred_log, "RAW OpenCL highlight reconstruct", stage_highlight_start);
    return;
  }

  if (cfa_pattern.kind == RawCfaKind::XTrans6x6) {
    const int  passes             = params.decode_res_ == DecodeRes::FULL ? 3 : 1;
    const auto stage_xtrans_start = ProfileClock::now();
    OpenCL::XTransToRGB_Ref(gpu_img, cfa_pattern.xtrans_pattern, passes);
    LogProfileStep(deferred_log, "RAW OpenCL xtrans interpolate", stage_xtrans_start);
  } else {
    const auto stage_debayer_start = ProfileClock::now();
    OpenCL::Bayer2x2ToRGB_RCD(gpu_img, cfa_pattern.bayer_pattern);
    LogProfileStep(deferred_log, "RAW OpenCL debayer", stage_debayer_start);
  }

  const auto     stage_crop_start = ProfileClock::now();
  const cv::Size crop_size(gpu_img.Width(), gpu_img.Height());
  const cv::Rect crop_rect =
      cfa_pattern.kind == RawCfaKind::Bayer2x2
          ? detail::BuildRcdDecodeCropRect(sizes, default_crop, crop_size, params.decode_res_)
          : detail::BuildDecodeCropRect(sizes, default_crop, crop_size, params.decode_res_);
  CropOpenClImage(gpu_img, crop_rect);
  LogProfileStep(deferred_log, "RAW OpenCL crop", stage_crop_start);
}

// Continue from demosaiced CV_32FC4 camera RGB through inverse cam-mul, optional DNG warp,
// and orientation. Shared by Neural success and Legacy paths.
void FinishOpenClCameraRgb(opencl::OpenClImage& gpu_img, const float* cam_mul,
                           const std::optional<dng::WarpRectilinear>& dng_warp,
                           RawRuntimeColorContext& runtime_color_context, const int flip,
                           DeferredOpenClLog& deferred_log) {
  const auto stage_cam_mul_start = ProfileClock::now();
  OpenCL::ApplyInverseCamMul(gpu_img, cam_mul);
  LogProfileStep(deferred_log, "RAW OpenCL apply inverse cam mul", stage_cam_mul_start);

  if (dng_warp.has_value()) {
    const auto          stage_dng_warp_start = ProfileClock::now();
    opencl::OpenClImage warped;
    OpenCL::Geometry::WarpRectilinear(gpu_img, warped, *dng_warp);
    gpu_img                                              = std::move(warped);
    runtime_color_context.dng_warp_rectilinear_applied_ = true;
    LogProfileStep(deferred_log, "RAW OpenCL DNG warp rectilinear", stage_dng_warp_start);
  }

  runtime_color_context.output_in_camera_space_ = true;
  const auto stage_geo_start                     = ProfileClock::now();
  ApplyOpenClGeometricCorrections(gpu_img, flip);
  LogProfileStep(deferred_log, "RAW OpenCL geometric corrections", stage_geo_start);
}

}  // namespace

auto RawProcessor::ProcessDirectRgbOpenCL() -> ImageBuffer {
  process_buffer_.SyncToGPU(GpuBackendKind::OpenCL);
  process_buffer_.ReleaseCPUData();
  auto& gpu_img = process_buffer_.GetOpenClImage();
  ApplyOpenClGeometricCorrections(gpu_img, raw_data_.sizes.flip);
  return {std::move(process_buffer_)};
}

auto RawProcessor::ProcessOpenCL() -> ImageBuffer {
  if (input_kind_ == RawInputKind::DebayeredRgb) {
    return ProcessDirectRgbOpenCL();
  }

  DeferredOpenClLog deferred_log;
  const auto        full_frame_start       = ProfileClock::now();

  // CPU downsample (same as Metal / CUDA paths).
  const auto        stage_decode_res_start = ProfileClock::now();
  SetDecodeRes(gpu_input_downsample_passes_);
  LogProfileStep(deferred_log, "RAW OpenCL setup decode-res", stage_decode_res_start);

  const auto stage_upload_start = ProfileClock::now();
  process_buffer_.SyncToGPU(GpuBackendKind::OpenCL);
  process_buffer_.ReleaseCPUData();
  LogProfileStep(deferred_log, "RAW OpenCL sync/upload", stage_upload_start);

  auto&      gpu_img            = process_buffer_.GetOpenClImage();

  const auto stage_linear_start = ProfileClock::now();
  OpenCL::ToLinearRef(gpu_img, raw_processor_, cfa_pattern_);
  LogProfileStep(deferred_log, "RAW OpenCL to-linear", stage_linear_start);

  const RawDemosaicMethod demosaic_method =
      detail::ResolveRawDemosaicMethod(params_, cfa_pattern_.kind);

  if (demosaic_method == RawDemosaicMethod::NeuralEngine) {
    // Match CUDA product sandwich: HLR-off clamps linear CFA before Neural preprocess.
    // The monochrome linear buffer is never replaced until Neural commits RGBA, so Legacy
    // soft-fail always reuses the same OpenCL CFA the path started with.
    if (!params_.highlights_reconstruct_) {
      const auto stage_clamp_start = ProfileClock::now();
      OpenCL::Clamp01(gpu_img);
      LogProfileStep(deferred_log, "RAW OpenCL clamp", stage_clamp_start);
    }

    const auto             stage_neural_start = ProfileClock::now();
    const cv::Size         original_cfa_size(gpu_img.Width(), gpu_img.Height());
    // Reusable aligned-RGBA staging across ProcessOpenCL calls (product path is
    // single-active-decode). Avoids a fresh clCreateBuffer every hot decode for the
    // full aligned Neural canvas; crop still materialises product-owned decode size.
    static opencl::OpenClImage neural_rgba_staging;
    OpenCL::OpenClNeuralDemosaicOptions neural_options;
    const auto neural_result = OpenCL::DemosaicWithNeuralEngine(
        gpu_img, cfa_pattern_, neural_rgba_staging, neural_options);

    if (neural_result.succeeded) {
      LogProfileStep(deferred_log, "RAW OpenCL Neural Engine demosaic", stage_neural_start);

      const detail::NeuralOutputGeometry geometry =
          detail::MakeStudentTiledNeuralOutputGeometry(
              neural_result.phase_shift_x, neural_result.phase_shift_y,
              cv::Size(neural_result.aligned_width, neural_result.aligned_height));
      const cv::Rect crop_rect = detail::BuildNeuralEngineDecodeCropRect(
          raw_data_.sizes, default_crop_, original_cfa_size, params_.decode_res_, geometry);
      // Crop into the process buffer (overwrites linear CFA after Neural commit).
      // Staging keeps the full aligned canvas for the next hot decode.
      OpenCL::Geometry::CropResize(neural_rgba_staging, gpu_img, crop_rect, crop_rect.size());

      if (params_.highlights_reconstruct_) {
        const auto stage_highlight_start = ProfileClock::now();
        OpenCL::HighlightReconstruct(gpu_img, raw_processor_);
        LogProfileStep(deferred_log, "RAW OpenCL highlight reconstruct", stage_highlight_start);
      }

      FinishOpenClCameraRgb(gpu_img, raw_data_.color.cam_mul, dng_warp_rectilinear_,
                            runtime_color_context_, raw_data_.sizes.flip, deferred_log);
      PrintProfileMs(deferred_log, "RAW OpenCL FullFrame", ProfileClock::now() - full_frame_start);
      deferred_log.Flush();
      return {std::move(process_buffer_)};
    }

    // Soft-fail: discard partial Neural output and stay on OpenCL Legacy.
    OpenCL::NoteOpenClNeuralLegacyFallbackForTest();
    const std::string failure_stage =
        neural_result.failure_stage.empty() ? "unknown" : neural_result.failure_stage;
    const std::string variant = neural_result.variant.empty() ? "unknown" : neural_result.variant;
    deferred_log.Add("RAW OpenCL Neural Engine unavailable (stage=" + failure_stage +
                     ", variant=" + variant + "); using OpenCL Legacy: " + neural_result.error);
  }

  RunOpenClLegacyDemosaic(gpu_img, cfa_pattern_, params_, raw_processor_, raw_data_.sizes,
                          default_crop_, deferred_log);
  FinishOpenClCameraRgb(gpu_img, raw_data_.color.cam_mul, dng_warp_rectilinear_,
                        runtime_color_context_, raw_data_.sizes.flip, deferred_log);

  PrintProfileMs(deferred_log, "RAW OpenCL FullFrame", ProfileClock::now() - full_frame_start);
  deferred_log.Flush();
  return {std::move(process_buffer_)};
}

}  // namespace alcedo

#endif
