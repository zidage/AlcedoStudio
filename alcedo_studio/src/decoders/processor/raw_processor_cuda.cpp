//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/raw_processor.hpp"

#ifdef HAVE_CUDA

#include <cuda_runtime_api.h>

#include <chrono>
#include <iostream>
#include <opencv2/core/cuda.hpp>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "decoders/processor/operators/gpu/cuda_color_space_conv.hpp"
#include "decoders/processor/operators/gpu/cuda_debayer_rcd.hpp"
#include "decoders/processor/nn/demosaicnet_preprocess.hpp"
#include "decoders/processor/operators/gpu/cuda_demosaicnet.hpp"
#include "decoders/processor/operators/gpu/cuda_dng_warp.hpp"
#include "decoders/processor/operators/gpu/cuda_downsample.hpp"
#include "decoders/processor/operators/gpu/cuda_highlight_reconstruct.hpp"
#include "decoders/processor/operators/gpu/cuda_rotate.hpp"
#include "decoders/processor/operators/gpu/cuda_white_balance.hpp"
#include "decoders/processor/operators/gpu/cuda_xtrans_interpolate.hpp"
#include "decoders/processor/raw_processor_internal.hpp"

namespace alcedo {
namespace {

using ProfileClock                 = std::chrono::steady_clock;

constexpr int kRcdOutputCropRadius = 4;

struct DeferredCudaLog {
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

thread_local DeferredCudaLog* g_deferred_cuda_log = nullptr;

class ScopedDeferredCudaLog {
 public:
  explicit ScopedDeferredCudaLog(DeferredCudaLog& log) : prev_(g_deferred_cuda_log) {
    g_deferred_cuda_log = &log;
  }

  ~ScopedDeferredCudaLog() { g_deferred_cuda_log = prev_; }

 private:
  DeferredCudaLog* prev_ = nullptr;
};

void AppendDeferredLog(std::string entry) {
  if (g_deferred_cuda_log != nullptr) {
    g_deferred_cuda_log->Add(std::move(entry));
    return;
  }

  std::cout << "[LOG] " << entry << '\n';
}

void PrintProfileMs(const char* label, const ProfileClock::duration elapsed) {
  std::ostringstream oss;
  oss << label << '=' << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
      << " ms";
  AppendDeferredLog(oss.str());
}

void LogCpuProfileStep(const char* label, const ProfileClock::time_point start) {
  PrintProfileMs(label, ProfileClock::now() - start);
}

void LogCudaProfileStep(cv::cuda::Stream& stream, const char* label,
                        const ProfileClock::time_point start) {
  stream.waitForCompletion();
  PrintProfileMs(label, ProfileClock::now() - start);
}

void LogVramUsage(const char* tag) {
  size_t            free_bytes  = 0;
  size_t            total_bytes = 0;
  const cudaError_t err         = cudaMemGetInfo(&free_bytes, &total_bytes);
  if (err != cudaSuccess) {
    std::ostringstream oss;
    oss << "VRAM " << tag << ": cudaMemGetInfo failed: " << cudaGetErrorString(err);
    AppendDeferredLog(oss.str());
    return;
  }
  const size_t       used_bytes = total_bytes - free_bytes;
  std::ostringstream oss;
  oss << "VRAM " << tag << ": free=" << (free_bytes >> 20) << " MB, total=" << (total_bytes >> 20)
      << " MB, used=" << (used_bytes >> 20) << " MB";
  AppendDeferredLog(oss.str());
}

auto DecodeResToDownsamplePasses(const DecodeRes decode_res) -> int {
  switch (decode_res) {
    case DecodeRes::FULL:
      return 0;
    case DecodeRes::HALF:
      return 1;
    case DecodeRes::QUARTER:
      return 2;
    case DecodeRes::EIGHTH:
      return 3;
    default:
      throw std::runtime_error("RawProcessor: Unknown decode resolution");
  }
}

void NormalizeDecodeResForGpu(const cv::Size& image_size, RawParams& params) {
  const int long_side = std::max(image_size.width, image_size.height);
  if (long_side > 8500 && params.decode_res_ == DecodeRes::QUARTER) {
    params.decode_res_ = DecodeRes::EIGHTH;
  }
}

struct CudaTileJob {
  cv::Rect source_rect;
  cv::Rect inner_rect_in_tile;
  cv::Rect output_rect;
};

auto ShiftRect(const cv::Rect& rect, const int dx, const int dy) -> cv::Rect {
  return {rect.x + dx, rect.y + dy, rect.width, rect.height};
}

auto RcdCroppedTileRect(const cv::Rect& rect) -> cv::Rect {
  return {rect.x - kRcdOutputCropRadius, rect.y - kRcdOutputCropRadius, rect.width, rect.height};
}

auto ShiftBayerPattern(const BayerPattern2x2& pattern, const int y_offset, const int x_offset)
    -> BayerPattern2x2 {
  BayerPattern2x2 shifted = {};
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) {
      const int idx       = BayerCellIndex(y, x);
      shifted.raw_fc[idx] = RawColorAt(pattern, y + y_offset, x + x_offset);
      shifted.rgb_fc[idx] = RgbColorAt(pattern, y + y_offset, x + x_offset);
    }
  }
  return shifted;
}

auto BuildTileJobs(const cv::Rect& active_rect, const cv::Size& full_size)
    -> std::vector<CudaTileJob> {
  std::vector<CudaTileJob> jobs;
  for (int y = 0; y < active_rect.height; y += detail::kCudaTileInnerSize) {
    const int inner_h = std::min(detail::kCudaTileInnerSize, active_rect.height - y);
    for (int x = 0; x < active_rect.width; x += detail::kCudaTileInnerSize) {
      const int      inner_w = std::min(detail::kCudaTileInnerSize, active_rect.width - x);

      const cv::Rect inner_abs(active_rect.x + x, active_rect.y + y, inner_w, inner_h);
      const int      src_x = std::max(0, inner_abs.x - detail::kCudaTileHaloSize);
      const int      src_y = std::max(0, inner_abs.y - detail::kCudaTileHaloSize);
      const int      src_r =
          std::min(full_size.width, inner_abs.x + inner_abs.width + detail::kCudaTileHaloSize);
      const int src_b =
          std::min(full_size.height, inner_abs.y + inner_abs.height + detail::kCudaTileHaloSize);

      const cv::Rect source_rect(src_x, src_y, src_r - src_x, src_b - src_y);
      jobs.push_back({
          .source_rect        = source_rect,
          .inner_rect_in_tile = cv::Rect(inner_abs.x - source_rect.x, inner_abs.y - source_rect.y,
                                         inner_abs.width, inner_abs.height),
          .output_rect        = cv::Rect(x, y, inner_abs.width, inner_abs.height),
      });
    }
  }
  return jobs;
}

void ApplyCudaGeometricCorrections(cv::cuda::GpuMat& gpu_img, const int flip,
                                   cv::cuda::Stream* stream) {
  switch (flip) {
    case 3:
      CUDA::Rotate180(gpu_img, stream);
      break;
    case 5:
      CUDA::Rotate90CCW(gpu_img, stream);
      break;
    case 6:
      CUDA::Rotate90CW(gpu_img, stream);
      break;
    default:
      break;
  }
}

}  // namespace

auto RawProcessor::ProcessCudaFullFrame() -> ImageBuffer {
  const auto               full_frame_start = ProfileClock::now();
  cv::cuda::Stream         stream;
  CUDA::RcdWorkspace       rcd_workspace;
  CUDA::HighlightWorkspace highlight_workspace;
  cv::cuda::GpuMat         output_rgba;

  const auto               stage_upload_start = ProfileClock::now();
  process_buffer_.SyncToGPU();
  process_buffer_.ReleaseCPUData();
  LogCpuProfileStep("RAW CUDA FullFrame sync/upload", stage_upload_start);

  auto&      gpu_img                = process_buffer_.GetCUDAImage();

  const auto stage_downsample_start = ProfileClock::now();
  CUDA::DownsampleRaw(
      gpu_img, cfa_pattern_,
      std::max(0, DecodeResToDownsamplePasses(params_.decode_res_) - gpu_input_downsample_passes_),
      &stream);
  LogCudaProfileStep(stream, "RAW CUDA FullFrame downsample", stage_downsample_start);

  const auto stage_linear_start = ProfileClock::now();
  CUDA::ToLinearRef(gpu_img, raw_processor_, cfa_pattern_, &stream);
  LogCudaProfileStep(stream, "RAW CUDA FullFrame to-linear", stage_linear_start);

  const RawDemosaicMethod demosaic_method =
      detail::ResolveRawDemosaicMethod(params_, cfa_pattern_.kind);
  if (demosaic_method == RawDemosaicMethod::NeuralEngine) {
    // HLR-on: keep over-range CFA samples. HLR-off: product Clamp01 before NN preprocess.
    if (!params_.highlights_reconstruct_) {
      const auto stage_clamp_start = ProfileClock::now();
      CUDA::Clamp01(gpu_img, &stream);
      LogCudaProfileStep(stream, "RAW CUDA FullFrame clamp", stage_clamp_start);
    }

    const auto            stage_neural_start = ProfileClock::now();
    const cv::Size        original_cfa_size  = gpu_img.size();
    cv::cuda::GpuMat      neural_cfa;
    const NeuralEngineCfaPrep prep =
        PrepareNeuralEngineCfa(gpu_img, cfa_pattern_, neural_cfa, &stream);
    if (!prep.succeeded) {
      AppendDeferredLog("RAW CUDA Neural Engine preprocess unavailable; using Legacy: " +
                        prep.error);
    } else {
      cv::cuda::GpuMat neural_rgb;
      const auto       neural_result =
          CUDA::DemosaicWithNeuralEngine(neural_cfa, prep.aligned_pattern, neural_rgb, &stream);
      if (!neural_result.succeeded) {
        AppendDeferredLog("RAW CUDA Neural Engine unavailable; using Legacy: " +
                          neural_result.error);
      } else {
        // Gamma decode back to linear camera RGB before crop / inverse cam-mul / HLR.
        FinishNeuralEngineRgb(neural_rgb, &stream);
        gpu_img = std::move(neural_rgb);
        LogCpuProfileStep("RAW CUDA FullFrame Neural Engine demosaic", stage_neural_start);

        const cv::Rect crop_rect = detail::BuildNeuralEngineDecodeCropRect(
            raw_data_.sizes, default_crop_, original_cfa_size, gpu_img.size(), params_.decode_res_,
            neural_result.source_border, prep.shift.sx, prep.shift.sy);
        if (!detail::IsFullImageRect(crop_rect, gpu_img.size())) {
          gpu_img = gpu_img(crop_rect);
        }

        if (params_.highlights_reconstruct_) {
          CUDA::HighlightCorrection   correction = CUDA::BuildHighlightCorrection(raw_processor_);
          CUDA::HighlightAccumulation accumulation;
          CUDA::AccumulateHighlightStats(gpu_img, correction, cv::Rect{}, highlight_workspace,
                                         accumulation, &stream);
          CUDA::FinalizeHighlightCorrection(accumulation, correction);

          if (dng_warp_rectilinear_.has_value()) {
            CUDA::ApplyHighlightCorrectionAndPackRGBA(gpu_img, output_rgba, correction,
                                                      raw_data_.color.cam_mul, &highlight_workspace,
                                                      &stream);
            CUDA::ApplyDngWarpRectilinear(output_rgba, *dng_warp_rectilinear_, &stream);
            runtime_color_context_.dng_warp_rectilinear_applied_ = true;
            ApplyCudaGeometricCorrections(output_rgba, raw_data_.sizes.flip, &stream);
          } else {
            CUDA::ApplyHighlightCorrectionAndPackRGBAOriented(
                gpu_img, output_rgba, correction, raw_data_.color.cam_mul, raw_data_.sizes.flip,
                &highlight_workspace, &stream);
          }
        } else if (dng_warp_rectilinear_.has_value()) {
          CUDA::ApplyInverseCamMulAndPackRGBA(gpu_img, output_rgba, raw_data_.color.cam_mul,
                                              &stream);
          CUDA::ApplyDngWarpRectilinear(output_rgba, *dng_warp_rectilinear_, &stream);
          runtime_color_context_.dng_warp_rectilinear_applied_ = true;
          ApplyCudaGeometricCorrections(output_rgba, raw_data_.sizes.flip, &stream);
        } else {
          CUDA::ApplyInverseCamMulAndPackRGBAOriented(gpu_img, output_rgba, raw_data_.color.cam_mul,
                                                      raw_data_.sizes.flip, &stream);
        }
        stream.waitForCompletion();

        runtime_color_context_.output_in_camera_space_ = true;
        process_buffer_                                = {std::move(output_rgba)};
        PrintProfileMs("RAW CUDA FullFrame", ProfileClock::now() - full_frame_start);
        return {std::move(process_buffer_)};
      }
    }
  }

  if (cfa_pattern_.kind == RawCfaKind::Bayer2x2 && params_.highlights_reconstruct_) {
    const auto stage_debayer_start = ProfileClock::now();
    CUDA::Bayer2x2ToPlanarRGB_RCD(gpu_img, cfa_pattern_.bayer_pattern, &rcd_workspace, &stream);
    LogCudaProfileStep(stream, "RAW CUDA FullFrame debayer", stage_debayer_start);

    cv::cuda::GpuMat debayer_r = rcd_workspace.r;
    cv::cuda::GpuMat debayer_g = rcd_workspace.g;
    cv::cuda::GpuMat debayer_b = rcd_workspace.b;
    const cv::Rect   crop_rect =
        detail::BuildRcdDecodeCropRect(raw_data_.sizes, default_crop_, debayer_r.size(),
                                       params_.decode_res_, kRcdOutputCropRadius);
    const auto stage_crop_start = ProfileClock::now();
    if (!detail::IsFullImageRect(crop_rect, debayer_r.size())) {
      debayer_r = debayer_r(crop_rect);
      debayer_g = debayer_g(crop_rect);
      debayer_b = debayer_b(crop_rect);
    }
    LogCpuProfileStep("RAW CUDA FullFrame crop", stage_crop_start);

    CUDA::HighlightCorrection   correction = CUDA::BuildHighlightCorrection(raw_processor_);
    CUDA::HighlightAccumulation accumulation;
    const auto                  stage_highlight_stats_start = ProfileClock::now();
    CUDA::AccumulateHighlightStats(debayer_r, debayer_g, debayer_b, correction, cv::Rect{},
                                   highlight_workspace, accumulation, &stream);
    CUDA::FinalizeHighlightCorrection(accumulation, correction);
    LogCpuProfileStep("RAW CUDA FullFrame highlight stats", stage_highlight_stats_start);

    const auto stage_highlight_start = ProfileClock::now();
    if (dng_warp_rectilinear_.has_value()) {
      CUDA::ApplyHighlightCorrectionAndPackRGBA(debayer_r, debayer_g, debayer_b, output_rgba,
                                                correction, raw_data_.color.cam_mul,
                                                &highlight_workspace, &stream);
      CUDA::ApplyDngWarpRectilinear(output_rgba, *dng_warp_rectilinear_, &stream);
      runtime_color_context_.dng_warp_rectilinear_applied_ = true;
      ApplyCudaGeometricCorrections(output_rgba, raw_data_.sizes.flip, &stream);
    } else {
      CUDA::ApplyHighlightCorrectionAndPackRGBAOriented(
          debayer_r, debayer_g, debayer_b, output_rgba, correction, raw_data_.color.cam_mul,
          raw_data_.sizes.flip, &highlight_workspace, &stream);
    }
    LogCudaProfileStep(stream, "RAW CUDA FullFrame highlight reconstruct + warp + pack rgba",
                       stage_highlight_start);

    runtime_color_context_.output_in_camera_space_ = true;

    process_buffer_                                = {std::move(output_rgba)};
    PrintProfileMs("RAW CUDA FullFrame", ProfileClock::now() - full_frame_start);
    return {std::move(process_buffer_)};
  } else {
    const auto stage_clamp_start = ProfileClock::now();
    CUDA::Clamp01(gpu_img, &stream);
    LogCudaProfileStep(stream, "RAW CUDA FullFrame clamp", stage_clamp_start);

    if (cfa_pattern_.kind == RawCfaKind::XTrans6x6) {
      const int  passes             = params_.decode_res_ == DecodeRes::FULL ? 3 : 1;
      const auto stage_xtrans_start = ProfileClock::now();
      stream.waitForCompletion();
      CUDA::XTransToRGB_Ref(gpu_img, cfa_pattern_.xtrans_pattern, passes);
      LogCpuProfileStep("RAW CUDA FullFrame xtrans interpolate", stage_xtrans_start);
    } else {
      const auto stage_debayer_start = ProfileClock::now();
      CUDA::Bayer2x2ToRGB_RCD(gpu_img, cfa_pattern_.bayer_pattern, &rcd_workspace, &stream);
      LogCudaProfileStep(stream, "RAW CUDA FullFrame debayer", stage_debayer_start);
    }

    const cv::Rect crop_rect =
        cfa_pattern_.kind == RawCfaKind::Bayer2x2
            ? detail::BuildRcdDecodeCropRect(raw_data_.sizes, default_crop_, gpu_img.size(),
                                             params_.decode_res_, kRcdOutputCropRadius)
            : detail::BuildDecodeCropRect(raw_data_.sizes, default_crop_, gpu_img.size(),
                                          params_.decode_res_);
    const auto stage_crop_start = ProfileClock::now();
    if (!detail::IsFullImageRect(crop_rect, gpu_img.size())) {
      gpu_img = gpu_img(crop_rect);
    }
    LogCpuProfileStep("RAW CUDA FullFrame crop", stage_crop_start);
  }

  const auto stage_pack_start = ProfileClock::now();
  if (dng_warp_rectilinear_.has_value()) {
    CUDA::ApplyInverseCamMulAndPackRGBA(gpu_img, output_rgba, raw_data_.color.cam_mul, &stream);
    CUDA::ApplyDngWarpRectilinear(output_rgba, *dng_warp_rectilinear_, &stream);
    runtime_color_context_.dng_warp_rectilinear_applied_ = true;
    ApplyCudaGeometricCorrections(output_rgba, raw_data_.sizes.flip, &stream);
  } else {
    CUDA::ApplyInverseCamMulAndPackRGBAOriented(gpu_img, output_rgba, raw_data_.color.cam_mul,
                                                raw_data_.sizes.flip, &stream);
  }
  LogCudaProfileStep(stream, "RAW CUDA FullFrame apply inverse cam mul + warp + pack rgba",
                     stage_pack_start);

  runtime_color_context_.output_in_camera_space_ = true;
  process_buffer_                                = {std::move(output_rgba)};

  PrintProfileMs("RAW CUDA FullFrame", ProfileClock::now() - full_frame_start);

  return {std::move(process_buffer_)};
}

auto RawProcessor::ProcessCudaTiled() -> ImageBuffer {
  const auto               tiled_start = ProfileClock::now();
  cv::cuda::Stream         stream;
  CUDA::RcdWorkspace       rcd_workspace;
  CUDA::HighlightWorkspace highlight_workspace;

  const auto               stage_upload_start = ProfileClock::now();
  process_buffer_.SyncToGPU();
  process_buffer_.ReleaseCPUData();
  LogCpuProfileStep("RAW CUDA Tiled sync/upload", stage_upload_start);

  auto&      linear_raw             = process_buffer_.GetCUDAImage();

  const auto stage_downsample_start = ProfileClock::now();
  CUDA::DownsampleRaw(
      linear_raw, cfa_pattern_,
      std::max(0, DecodeResToDownsamplePasses(params_.decode_res_) - gpu_input_downsample_passes_),
      &stream);
  LogCudaProfileStep(stream, "RAW CUDA Tiled downsample", stage_downsample_start);

  const auto stage_linear_start = ProfileClock::now();
  CUDA::ToLinearRef(linear_raw, raw_processor_, cfa_pattern_, &stream);
  LogCudaProfileStep(stream, "RAW CUDA Tiled to-linear", stage_linear_start);

  const auto     stage_jobs_start = ProfileClock::now();
  const cv::Size rcd_output_size(linear_raw.cols - 2 * kRcdOutputCropRadius,
                                 linear_raw.rows - 2 * kRcdOutputCropRadius);
  if (rcd_output_size.width <= 0 || rcd_output_size.height <= 0) {
    throw std::runtime_error("RawProcessor: CUDA tiled RCD input is too small.");
  }
  const cv::Rect active_rect = detail::BuildRcdDecodeCropRect(
      raw_data_.sizes, default_crop_, rcd_output_size, params_.decode_res_, kRcdOutputCropRadius);
  const cv::Rect tile_active_rect =
      ShiftRect(active_rect, kRcdOutputCropRadius, kRcdOutputCropRadius);
  auto jobs = BuildTileJobs(tile_active_rect, linear_raw.size());
  LogCpuProfileStep("RAW CUDA Tiled build tile jobs", stage_jobs_start);

  CUDA::HighlightCorrection   correction = CUDA::BuildHighlightCorrection(raw_processor_);
  CUDA::HighlightAccumulation accumulation;
  cv::cuda::GpuMat            output_rgba;

  if (params_.highlights_reconstruct_) {
    const auto       stage_highlight_stats_start = ProfileClock::now();
    cv::cuda::GpuMat tile_raw;
    for (const auto& job : jobs) {
      linear_raw(job.source_rect).copyTo(tile_raw, stream);
      const BayerPattern2x2 tile_pattern =
          ShiftBayerPattern(cfa_pattern_.bayer_pattern, job.source_rect.y, job.source_rect.x);
      CUDA::Bayer2x2ToPlanarRGB_RCD(tile_raw, tile_pattern, &rcd_workspace, &stream);
      const cv::Rect rcd_inner_rect = RcdCroppedTileRect(job.inner_rect_in_tile);
      CUDA::AccumulateHighlightStats(rcd_workspace.r, rcd_workspace.g, rcd_workspace.b, correction,
                                     rcd_inner_rect, highlight_workspace, accumulation, &stream);
    }
    CUDA::FinalizeHighlightCorrection(accumulation, correction);
    LogCudaProfileStep(stream, "RAW CUDA Tiled highlight stats", stage_highlight_stats_start);
  }

  const auto       stage_tiles_start = ProfileClock::now();
  cv::cuda::GpuMat tile_raw;
  if (params_.highlights_reconstruct_) {
    output_rgba.create(active_rect.height, active_rect.width, CV_32FC4);
    cv::cuda::GpuMat tile_rgba;
    for (const auto& job : jobs) {
      linear_raw(job.source_rect).copyTo(tile_raw, stream);
      const BayerPattern2x2 tile_pattern =
          ShiftBayerPattern(cfa_pattern_.bayer_pattern, job.source_rect.y, job.source_rect.x);
      CUDA::Bayer2x2ToPlanarRGB_RCD(tile_raw, tile_pattern, &rcd_workspace, &stream);
      CUDA::ApplyHighlightCorrectionAndPackRGBA(rcd_workspace.r, rcd_workspace.g, rcd_workspace.b,
                                                tile_rgba, correction, raw_data_.color.cam_mul,
                                                &highlight_workspace, &stream);
      tile_rgba(RcdCroppedTileRect(job.inner_rect_in_tile))
          .copyTo(output_rgba(job.output_rect), stream);
    }
    LogCudaProfileStep(stream, "RAW CUDA Tiled highlight reconstruct + pack tile assembly",
                       stage_tiles_start);

    const auto stage_geo_start = ProfileClock::now();
    if (dng_warp_rectilinear_.has_value()) {
      CUDA::ApplyDngWarpRectilinear(output_rgba, *dng_warp_rectilinear_, &stream);
      runtime_color_context_.dng_warp_rectilinear_applied_ = true;
    }
    ApplyCudaGeometricCorrections(output_rgba, raw_data_.sizes.flip, &stream);
    LogCudaProfileStep(stream, "RAW CUDA Tiled geometric corrections", stage_geo_start);
  } else {
    cv::cuda::GpuMat output_rgb;
    output_rgb.create(active_rect.height, active_rect.width, CV_32FC3);
    for (const auto& job : jobs) {
      linear_raw(job.source_rect).copyTo(tile_raw, stream);
      const BayerPattern2x2 tile_pattern =
          ShiftBayerPattern(cfa_pattern_.bayer_pattern, job.source_rect.y, job.source_rect.x);
      CUDA::Clamp01(tile_raw, &stream);
      CUDA::Bayer2x2ToRGB_RCD(tile_raw, tile_pattern, &rcd_workspace, &stream);
      tile_raw(RcdCroppedTileRect(job.inner_rect_in_tile))
          .copyTo(output_rgb(job.output_rect), stream);
    }
    LogCudaProfileStep(stream, "RAW CUDA Tiled tile assembly", stage_tiles_start);

    const auto stage_geo_start = ProfileClock::now();
    if (dng_warp_rectilinear_.has_value()) {
      CUDA::ApplyDngWarpRectilinear(output_rgb, *dng_warp_rectilinear_, &stream);
      runtime_color_context_.dng_warp_rectilinear_applied_ = true;
    }
    ApplyCudaGeometricCorrections(output_rgb, raw_data_.sizes.flip, &stream);
    LogCudaProfileStep(stream, "RAW CUDA Tiled geometric corrections", stage_geo_start);

    const auto stage_pack_start = ProfileClock::now();
    output_rgba.create(output_rgb.size(), CV_32FC4);
    CUDA::ApplyInverseCamMulAndPackRGBA(output_rgb, output_rgba, raw_data_.color.cam_mul, &stream);
    LogCudaProfileStep(stream, "RAW CUDA Tiled apply inverse cam mul + pack rgba",
                       stage_pack_start);
  }

  runtime_color_context_.output_in_camera_space_ = true;
  process_buffer_                                = {std::move(output_rgba)};
  PrintProfileMs("RAW CUDA Tiled", ProfileClock::now() - tiled_start);
  return {std::move(process_buffer_)};
}

auto RawProcessor::ProcessDirectRgbCuda() -> ImageBuffer {
  cv::cuda::Stream stream;
  process_buffer_.SyncToGPU();
  process_buffer_.ReleaseCPUData();
  auto& gpu_img = process_buffer_.GetCUDAImage();
  ApplyCudaGeometricCorrections(gpu_img, raw_data_.sizes.flip, &stream);
  stream.waitForCompletion();
  return {std::move(process_buffer_)};
}

auto RawProcessor::ProcessCuda() -> ImageBuffer {
  const auto                  start = ProfileClock::now();
  DeferredCudaLog             deferred_log;
  const ScopedDeferredCudaLog scoped_log(deferred_log);
  LogVramUsage("ProcessCuda ENTER");

  if (input_kind_ == RawInputKind::DebayeredRgb) {
    auto       out = ProcessDirectRgbCuda();
    const auto end = ProfileClock::now();
    PrintProfileMs("RAW decoding", end - start);
    LogVramUsage("ProcessCuda EXIT (DebayeredRgb)");
    deferred_log.Flush();
    return out;
  }

  auto&      cpu_data               = process_buffer_.GetCPUData();
  const auto stage_decode_res_start = ProfileClock::now();
  NormalizeDecodeResForGpu(cpu_data.size(), params_);
  LogCpuProfileStep("RAW CUDA setup decode-res", stage_decode_res_start);

  const auto     stage_mode_select_start = ProfileClock::now();
  const cv::Rect active_rect = detail::BuildDecodeCropRect(raw_data_.sizes, default_crop_,
                                                           cpu_data.size(), params_.decode_res_);
  const detail::CudaExecutionMode mode =
      detail::SelectCudaExecutionMode(params_, cfa_pattern_, active_rect);
  LogCpuProfileStep("RAW CUDA setup mode-select", stage_mode_select_start);

  ImageBuffer out =
      mode == detail::CudaExecutionMode::Tiled ? ProcessCudaTiled() : ProcessCudaFullFrame();
  const auto end = ProfileClock::now();
  PrintProfileMs("RAW decoding", end - start);
  LogVramUsage(mode == detail::CudaExecutionMode::Tiled ? "ProcessCuda EXIT (Tiled)"
                                                        : "ProcessCuda EXIT (FullFrame)");
  deferred_log.Flush();
  return out;
}

}  // namespace alcedo

#endif
