//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/cuda/cuda_sensor_demosaic.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

#include <opencv2/core/cuda_stream_accessor.hpp>

#include "decoders/processor/nn/demosaicnet_cache.hpp"
#include "decoders/processor/nn/demosaicnet_preprocess.hpp"
#include "decoders/processor/neural_tile_jobs.hpp"
#include "decoders/processor/operators/gpu/cuda_color_space_conv.hpp"
#include "decoders/processor/operators/gpu/cuda_debayer_rcd.hpp"
#include "decoders/processor/operators/gpu/cuda_demosaicnet.hpp"
#include "decoders/processor/operators/gpu/cuda_highlight_reconstruct.hpp"
#include "decoders/processor/operators/gpu/cuda_image_ops.hpp"
#include "decoders/processor/operators/gpu/cuda_xtrans_interpolate.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"

namespace alcedo {
namespace {

DemosaicNetModelCache* g_neural_model_cache_for_test = nullptr;

auto AllocateTransient(CudaRenderWorkspace& workspace, std::size_t bytes) -> void* {
  return workspace.TransientBuffers().Allocate(bytes);
}

auto WrapF32C1(void* ptr, int width, int height) -> cv::cuda::GpuMat {
  return cv::cuda::GpuMat(height, width, CV_32FC1, ptr,
                          static_cast<std::size_t>(width) * sizeof(float));
}

auto WrapF32C3(void* ptr, int width, int height) -> cv::cuda::GpuMat {
  return cv::cuda::GpuMat(height, width, CV_32FC3, ptr,
                          static_cast<std::size_t>(width) * sizeof(float) * 3);
}

auto CropIfNeeded(cv::cuda::GpuMat plane, const RectI& crop) -> cv::cuda::GpuMat {
  if (crop.width <= 0 || crop.height <= 0) {
    return plane;
  }
  const int x = std::max(0, crop.x);
  const int y = std::max(0, crop.y);
  const int w = std::min(crop.width, plane.cols - x);
  const int h = std::min(crop.height, plane.rows - y);
  if (w <= 0 || h <= 0) {
    return plane;
  }
  if (x == 0 && y == 0 && w == plane.cols && h == plane.rows) {
    return plane;
  }
  return plane(cv::Rect(x, y, w, h));
}

void ThrowIfPackedExtentMismatch(const cv::cuda::GpuMat& packed, const cv::cuda::GpuMat& rgb,
                                 int orientation_flip, const char* label) {
  if (orientation_flip == 0 && (packed.cols != rgb.cols || packed.rows != rgb.rows)) {
    throw std::runtime_error(std::string("ExecuteCudaDevelop: ") + label +
                             " output extent does not match plan");
  }
}

void PackRgbWithOptionalHighlight(CudaRenderWorkspace& workspace, cv::cuda::GpuMat rgb,
                                  cv::cuda::GpuMat packed, const float* cam_mul, int orientation_flip,
                                  bool hlr, cv::cuda::Stream& stream) {
  if (!hlr) {
    CUDA::ApplyInverseCamMulAndPackRGBAOriented(rgb, packed, cam_mul, orientation_flip, &stream);
    return;
  }
  const int cw = rgb.cols;
  const int ch = rgb.rows;
  int*   anyclipped = static_cast<int*>(AllocateTransient(workspace, sizeof(int)));
  float* sums       = static_cast<float*>(AllocateTransient(workspace, sizeof(float) * 4));
  float* cnts       = static_cast<float*>(AllocateTransient(workspace, sizeof(float) * 4));
  void*  hlr_rgb    = AllocateTransient(workspace, static_cast<std::size_t>(cw) * ch * sizeof(float) * 3);
  CUDA::HighlightWorkspace highlight;
  highlight.BindExternal(anyclipped, sums, cnts, hlr_rgb, cw, ch);
  CUDA::HighlightCorrection   correction = CUDA::BuildHighlightCorrection(cam_mul);
  CUDA::HighlightAccumulation accumulation;
  CUDA::AccumulateHighlightStats(rgb, correction, cv::Rect{}, highlight, accumulation, &stream);
  CUDA::FinalizeHighlightCorrection(accumulation, correction);
  CUDA::ApplyHighlightCorrectionAndPackRGBAOriented(rgb, packed, correction, cam_mul,
                                                    orientation_flip, &highlight, &stream);
}

void DemosaicBayerRcd(CudaRenderWorkspace& workspace, const PreparedRawInput& input,
                      cv::cuda::GpuMat linear, cv::cuda::GpuMat packed, bool hlr,
                      cv::cuda::Stream& stream) {
  const int w = linear.cols;
  const int h = linear.rows;
  void* r_ptr  = AllocateTransient(workspace, static_cast<std::size_t>(w) * h * sizeof(float));
  void* g_ptr  = AllocateTransient(workspace, static_cast<std::size_t>(w) * h * sizeof(float));
  void* b_ptr  = AllocateTransient(workspace, static_cast<std::size_t>(w) * h * sizeof(float));
  void* vh_ptr = AllocateTransient(workspace, static_cast<std::size_t>(w) * h * sizeof(float));
  void* pq_ptr = AllocateTransient(workspace, static_cast<std::size_t>(w) * h * sizeof(float));

  CUDA::RcdWorkspace rcd;
  rcd.BindExternal(r_ptr, g_ptr, b_ptr, vh_ptr, pq_ptr, cv::Size(w, h));
  CUDA::Bayer2x2ToPlanarRGB_RCD(linear, input.cfa_pattern.bayer_pattern, &rcd, &stream);

  auto r = CropIfNeeded(rcd.r, input.demosaic_output_crop);
  auto g = CropIfNeeded(rcd.g, input.demosaic_output_crop);
  auto b = CropIfNeeded(rcd.b, input.demosaic_output_crop);
  if (input.sensor.orientation_flip == 0 && (packed.cols != r.cols || packed.rows != r.rows)) {
    throw std::runtime_error("ExecuteCudaDevelop: Bayer output extent does not match plan");
  }

  if (hlr) {
    const int cw = r.cols;
    const int ch = r.rows;
    int*   anyclipped = static_cast<int*>(AllocateTransient(workspace, sizeof(int)));
    float* sums       = static_cast<float*>(AllocateTransient(workspace, sizeof(float) * 4));
    float* cnts       = static_cast<float*>(AllocateTransient(workspace, sizeof(float) * 4));
    void*  hlr_rgb    = AllocateTransient(workspace, static_cast<std::size_t>(cw) * ch * sizeof(float) * 3);
    CUDA::HighlightWorkspace highlight;
    highlight.BindExternal(anyclipped, sums, cnts, hlr_rgb, cw, ch);
    CUDA::HighlightCorrection   correction = CUDA::BuildHighlightCorrection(input.linearization.cam_mul);
    CUDA::HighlightAccumulation accumulation;
    CUDA::AccumulateHighlightStats(r, g, b, correction, cv::Rect{}, highlight, accumulation, &stream);
    CUDA::FinalizeHighlightCorrection(accumulation, correction);
    CUDA::ApplyHighlightCorrectionAndPackRGBAOriented(r, g, b, packed, correction,
                                                      input.linearization.cam_mul,
                                                      input.sensor.orientation_flip, &highlight,
                                                      &stream);
    return;
  }

  void* merge_ptr =
      AllocateTransient(workspace, static_cast<std::size_t>(r.cols) * r.rows * sizeof(float) * 3);
  auto merged = WrapF32C3(merge_ptr, r.cols, r.rows);
  CUDA::MergeRGB(r, g, b, merged, &stream);
  CUDA::ApplyInverseCamMulAndPackRGBAOriented(merged, packed, input.linearization.cam_mul,
                                              input.sensor.orientation_flip, &stream);
}

void DemosaicXTransInterpolator(CudaRenderWorkspace& workspace, const PreparedRawInput& input,
                                cv::cuda::GpuMat linear, cv::cuda::GpuMat packed, bool hlr,
                                cv::cuda::Stream& stream) {
  const int w = linear.cols;
  const int h = linear.rows;
  void* green_ptr = AllocateTransient(workspace, static_cast<std::size_t>(w) * h * sizeof(float));
  void* rgb_ptr =
      AllocateTransient(workspace, static_cast<std::size_t>(w) * h * sizeof(float) * 3);
  auto green  = WrapF32C1(green_ptr, w, h);
  auto rgb    = WrapF32C3(rgb_ptr, w, h);
  const int passes = input.downsample_passes == 0 ? 3 : 1;
  CUDA::XTransToRGB_Ref(linear, green, rgb, input.cfa_pattern.xtrans_pattern, passes, &stream);
  auto cropped = CropIfNeeded(rgb, input.demosaic_output_crop);
  ThrowIfPackedExtentMismatch(packed, cropped, input.sensor.orientation_flip, "X-Trans");
  PackRgbWithOptionalHighlight(workspace, cropped, packed, input.linearization.cam_mul,
                               input.sensor.orientation_flip, hlr, stream);
}

void DemosaicNeuralEngine(CudaRenderDevice& device, const PreparedRawInput& input,
                          cv::cuda::GpuMat linear, cv::cuda::GpuMat packed, bool hlr,
                          cv::cuda::Stream& stream) {
  cv::cuda::GpuMat neural_cfa;
  const auto       prep = PrepareNeuralEngineCfa(linear, input.cfa_pattern, neural_cfa, &stream);
  if (!prep.succeeded) {
    throw std::runtime_error("ExecuteCudaDevelop: Neural Engine preprocess failed: " + prep.error);
  }

  const bool is_bayer = input.cfa_pattern.kind == RawCfaKind::Bayer2x2;
  const auto policy =
      is_bayer ? detail::MakeBayerStudentTilePolicy() : detail::MakeXTransStudentTilePolicy();
  cv::cuda::GpuMat output_rgb(neural_cfa.size(), CV_32FC3);

  auto&                         neural_workspace = device.NeuralDemosaicWorkspace();
  CUDA::NeuralDemosaicOptions   neural_options;
  neural_options.workspace = &neural_workspace;
  if (g_neural_model_cache_for_test != nullptr) {
    neural_options.model_cache = g_neural_model_cache_for_test;
    neural_options.load_options.model_dir = "alcedo-missing-demosaicnet-models";
  }

  const auto variant = is_bayer ? DemosaicNetVariant::Bayer : DemosaicNetVariant::XTrans;
  auto&      cache   = neural_options.model_cache != nullptr ? *neural_options.model_cache
                                                             : DemosaicNetModelCache::Instance();
  DemosaicNetLoadOptions load_options = neural_options.load_options;
  load_options.stream = cv::cuda::StreamAccessor::getStream(stream);
  if (!cache.EnsureLoaded(variant, load_options)) {
    throw std::runtime_error("ExecuteCudaDevelop: Neural Engine unavailable: " + cache.LastError());
  }

  const int tile_h = policy.input_tile.height;
  const int tile_w = policy.input_tile.width;
  neural_workspace.EnsureCapacity(variant, tile_h, tile_w,
                                  static_cast<std::size_t>(3) * static_cast<std::size_t>(tile_h) *
                                      static_cast<std::size_t>(tile_w));

  const auto jobs =
      detail::BuildTileJobs(cv::Rect(0, 0, neural_cfa.cols, neural_cfa.rows), neural_cfa.size(),
                            policy);
  cv::cuda::GpuMat tile_rgb;
  for (const auto& job : jobs) {
    const auto result = CUDA::EnqueueDemosaicStudentTileWithNeuralEngine(
        neural_cfa, job.input_origin, prep.aligned_pattern, tile_rgb, &stream, neural_options);
    if (!result.succeeded) {
      throw std::runtime_error("ExecuteCudaDevelop: Neural Engine tile failed: " + result.error);
    }
    tile_rgb(job.model_output_roi).copyTo(output_rgb(job.destination_roi), stream);
  }

  auto cropped = CropIfNeeded(output_rgb, input.demosaic_output_crop);
  ThrowIfPackedExtentMismatch(packed, cropped, input.sensor.orientation_flip, "Neural Engine");
  PackRgbWithOptionalHighlight(device.Workspace(), cropped, packed, input.linearization.cam_mul,
                               input.sensor.orientation_flip, hlr, stream);
}

}  // namespace

void SetDevelopNeuralModelCacheForTesting(DemosaicNetModelCache* cache) {
  g_neural_model_cache_for_test = cache;
}

void ExecuteCudaSensorDemosaicAndPack(CudaRenderDevice& device, const PreparedRawInput& input,
                                      const DevelopPayload& params, cv::cuda::GpuMat linear,
                                      cv::cuda::GpuMat packed, cv::cuda::Stream& stream) {
  const bool hlr = params.highlights_reconstruct;
  const auto method =
      ResolveDevelopDemosaicMethod(params, input.cfa_pattern.kind, input.downsample_passes);
  if (method == RawDemosaicMethod::NeuralEngine) {
    DemosaicNeuralEngine(device, input, linear, packed, hlr, stream);
    return;
  }
  if (input.cfa_pattern.kind == RawCfaKind::XTrans6x6) {
    DemosaicXTransInterpolator(device.Workspace(), input, linear, packed, hlr, stream);
    return;
  }
  DemosaicBayerRcd(device.Workspace(), input, linear, packed, hlr, stream);
}

}  // namespace alcedo
