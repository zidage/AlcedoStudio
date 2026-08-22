//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <opencv2/core/cuda_types.hpp>
#include <opencv2/core/cuda_stream_accessor.hpp>

#include <stdexcept>

#include "decoders/processor/raw_normalization.hpp"
#include "decoders/processor/operators/gpu/cuda_raw_proc_utils.hpp"
#include "decoders/processor/operators/gpu/cuda_white_balance.hpp"


namespace alcedo {
namespace CUDA {
namespace {

auto GetCudaStream(cv::cuda::Stream* stream) -> cudaStream_t {
  if (stream == nullptr) {
    return nullptr;
  }
  return cv::cuda::StreamAccessor::getStream(*stream);
}

void MaybeWait(cv::cuda::Stream* stream) {
  if (stream == nullptr) {
    CUDA_CHECK(cudaDeviceSynchronize());
  }
}

}  // namespace

struct WBParams {
  float black_level[4];
  float white_level[4];
  float wb_multipliers[4];
  int   apply_white_balance;
  int   _padding[3];
  int   black_tile_width;
  int   black_tile_height;
  float pattern_black[36];
};

__global__ void ToLinearRefKernel(cv::cuda::PtrStep<float> image, int width, int height,
                                  WBParams wb_params, RawCfaPattern pattern) {
  // Calculate the global x and y coordinates of the pixel for this thread
  const int col = blockIdx.x * blockDim.x + threadIdx.x;
  const int row = blockIdx.y * blockDim.y + threadIdx.y;

  // Boundary check to avoid processing out-of-bounds pixels
  if (col >= width || row >= height) {
    return;
  }

  // Determine the color channel (0, 1, 3, 2) for the current pixel based on its position.
  // This standard calculation assumes a 2x2 Bayer pattern (like RGGB, GRBG, etc.).
  // The 'bayer_pattern_offset' helps align to the specific pattern from LibRaw's `idata.filters`.
  // The LibRaw COLOR(row, col) macro can often be simplified to this.
  const int color_idx = RawColorAt(pattern, row, col);



  // --- Start Processing ---

  // 1. Load pixel value and convert to float for processing
  const float sample = image(row, col);

  // 2. Black Level Subtraction
  float pattern_black = 0.0f;
  if (wb_params.black_tile_width > 0 && wb_params.black_tile_height > 0) {
    const int tile_y = row % wb_params.black_tile_height;
    const int tile_x = col % wb_params.black_tile_width;
    pattern_black =
        wb_params.pattern_black[tile_y * wb_params.black_tile_width + tile_x];
  }
  const float black = wb_params.black_level[color_idx] + pattern_black;
  float       pixel_val =
      raw_norm::NormalizeSample(sample, black, wb_params.white_level[color_idx]);
  pixel_val *= raw_norm::RelativeWhiteBalanceMultiplier(
      wb_params.wb_multipliers, color_idx, wb_params.apply_white_balance != 0);

  image(row, col) = pixel_val;
}

__global__ void ToLinearRefFromU16Kernel(cv::cuda::PtrStep<ushort> src, cv::cuda::PtrStep<float> dst,
                                         int width, int height, WBParams wb_params,
                                         RawCfaPattern pattern) {
  const int col = blockIdx.x * blockDim.x + threadIdx.x;
  const int row = blockIdx.y * blockDim.y + threadIdx.y;
  if (col >= width || row >= height) {
    return;
  }

  const int   color_idx = RawColorAt(pattern, row, col);
  const float sample    = static_cast<float>(src(row, col));

  float pattern_black = 0.0f;
  if (wb_params.black_tile_width > 0 && wb_params.black_tile_height > 0) {
    const int tile_y = row % wb_params.black_tile_height;
    const int tile_x = col % wb_params.black_tile_width;
    pattern_black    = wb_params.pattern_black[tile_y * wb_params.black_tile_width + tile_x];
  }
  const float black = wb_params.black_level[color_idx] + pattern_black;
  float       pixel_val =
      raw_norm::NormalizeSample(sample, black, wb_params.white_level[color_idx]);
  pixel_val *= raw_norm::RelativeWhiteBalanceMultiplier(
      wb_params.wb_multipliers, color_idx, wb_params.apply_white_balance != 0);
  dst(row, col) = pixel_val;
}

auto MakeWbParams(const RawLinearizationParams& params) -> WBParams {
  WBParams wb_params = {};
  for (int c = 0; c < 4; ++c) {
    wb_params.black_level[c]    = params.black_level[c];
    wb_params.white_level[c]    = params.white_level[c];
    wb_params.wb_multipliers[c] = params.cam_mul[c];
  }
  wb_params.apply_white_balance = params.apply_as_shot_wb;
  wb_params.black_tile_width    = params.black_tile_width;
  wb_params.black_tile_height   = params.black_tile_height;
  for (int i = 0; i < 36; ++i) {
    wb_params.pattern_black[i] = params.pattern_black[i];
  }
  return wb_params;
}

static auto GetWBCoeff(const libraw_rawdata_t& raw_data) -> const float* {
  return raw_data.color.cam_mul;
}

void ToLinearRef(cv::cuda::GpuMat& image, LibRaw& raw_processor, const RawCfaPattern& pattern,
                 cv::cuda::Stream* stream) {
  const auto raw_curve = raw_norm::BuildLinearizationCurve(raw_processor.imgdata.rawdata);
  const auto wb        = GetWBCoeff(raw_processor.imgdata.rawdata);

  // Ensure the input GpuMat has the correct type
  CV_Assert(image.type() == CV_16UC1);

  image.convertTo(image, CV_32FC1);

  WBParams wb_params = {};
  for (int c = 0; c < 4; ++c) {
    wb_params.black_level[c]    = raw_curve.black_level[c];
    wb_params.white_level[c]    = raw_curve.white_level[c];
    wb_params.wb_multipliers[c] = wb[c];
  }
  wb_params.apply_white_balance = raw_processor.imgdata.color.as_shot_wb_applied != 1 ? 1 : 0;
  wb_params.black_tile_width    = 0;
  wb_params.black_tile_height   = 0;
  const int tile_width          = raw_processor.imgdata.rawdata.color.cblack[4];
  const int tile_height         = raw_processor.imgdata.rawdata.color.cblack[5];
  const int entries             = tile_width * tile_height;
  if (entries > 0 && entries <= 36) {
    wb_params.black_tile_width  = tile_width;
    wb_params.black_tile_height = tile_height;
    for (int i = 0; i < entries; ++i) {
      wb_params.pattern_black[i] =
          static_cast<float>(raw_processor.imgdata.rawdata.color.cblack[6 + i]);
    }
  }

  const dim3 threads_per_block(32, 32);
  const dim3 num_blocks((image.cols + threads_per_block.x - 1) / threads_per_block.x,
                        (image.rows + threads_per_block.y - 1) / threads_per_block.y);

  const cudaStream_t cuda_stream = GetCudaStream(stream);
  ToLinearRefKernel<<<num_blocks, threads_per_block, 0, cuda_stream>>>(
      image, image.cols, image.rows, wb_params, pattern);

  CUDA_CHECK(cudaGetLastError());
  MaybeWait(stream);
}

void ToLinearRef(const cv::cuda::GpuMat& src_u16, cv::cuda::GpuMat& dst_f32,
                 const RawLinearizationParams& params, const RawCfaPattern& pattern,
                 cv::cuda::Stream* stream) {
  if (src_u16.type() != CV_16UC1) {
    throw std::runtime_error("CUDA::ToLinearRef: src must be CV_16UC1");
  }
  if (dst_f32.type() != CV_32FC1 || dst_f32.size() != src_u16.size() || dst_f32.empty()) {
    throw std::runtime_error("CUDA::ToLinearRef: dst must be a preallocated CV_32FC1 of src size");
  }

  const WBParams wb_params         = MakeWbParams(params);
  const dim3     threads_per_block(32, 32);
  const dim3     num_blocks((src_u16.cols + threads_per_block.x - 1) / threads_per_block.x,
                            (src_u16.rows + threads_per_block.y - 1) / threads_per_block.y);
  const cudaStream_t cuda_stream = GetCudaStream(stream);
  ToLinearRefFromU16Kernel<<<num_blocks, threads_per_block, 0, cuda_stream>>>(
      src_u16, dst_f32, src_u16.cols, src_u16.rows, wb_params, pattern);
  CUDA_CHECK(cudaGetLastError());
  MaybeWait(stream);
}
};  // namespace CUDA
};  // namespace alcedo
