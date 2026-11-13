//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/operators/gpu/cuda_debayer_ahd.hpp"
#include "decoders/processor/operators/gpu/cuda_image_ops.hpp"
#include "decoders/processor/operators/gpu/cuda_raw_proc_utils.hpp"

#include <opencv2/core/cuda.hpp>
#include <opencv2/core/mat.hpp>

namespace alcedo {
namespace CUDA {

__constant__ int   remap_d[4] = {0, 1, 3, 2};

__global__ void G_FinalGeneration(cv::cuda::PtrStep<float> raw, cv::cuda::PtrStep<float> G_final,
                                  cv::cuda::PtrStep<float> R_final,
                                  cv::cuda::PtrStep<float> B_final, int width, int height) {
  // Kernel implementation goes here
  const int x = blockIdx.x * blockDim.x + threadIdx.x;  // col
  const int y = blockIdx.y * blockDim.y + threadIdx.y;  // row

  // Boundary check to avoid processing out-of-bounds pixels
  if (y >= height - 2 || x >= width - 2 || y < 2 || x < 2) {
    return;
  }

  const int color_idx = remap_d[(((y % 2) * 2) + (x % 2)) % 4];
  if (color_idx == 0 || color_idx == 2) {
    float center  = raw(y, x);
    float h_avg   = 0.5f * (raw(y, x - 1) + raw(y, x + 1));
    float h_diff  = 0.25f * (2.0f * center - raw(y, x - 2) - raw(y, x + 2));

    float v_avg   = 0.5f * (raw(y - 1, x) + raw(y + 1, x));
    float v_diff  = 0.25f * (2.0f * center - raw(y - 2, x) - raw(y + 2, x));

    float Dh      = std::abs(raw(y, x - 1) - raw(y, x + 1));
    float Dv      = std::abs(raw(y - 1, x) - raw(y + 1, x));

    G_final(y, x) = (Dh < Dv) ? (h_avg + h_diff) : (v_avg + v_diff);

    R_final(y, x) = color_idx == 0 ? center : 0.0f;
    B_final(y, x) = color_idx == 2 ? center : 0.0f;
  }
}

__global__ void R_B_FinalGeneration(cv::cuda::PtrStep<float> raw, cv::cuda::PtrStep<float> G_final,
                                    cv::cuda::PtrStep<float> R_final,
                                    cv::cuda::PtrStep<float> B_final, int width, int height) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;  // col
  const int y = blockIdx.y * blockDim.y + threadIdx.y;  // row

  // Boundary check to avoid processing out-of-bounds pixels
  if (y >= height - 1 || x >= width - 1 || y < 1 || x < 1) {
    return;
  }

  const int color_idx = remap_d[(((y % 2) * 2) + (x % 2)) % 4];
  if (color_idx != 0) {
    float estimate_R = 0.0f;

    if (color_idx == 1) {
      float left  = R_final(y, x - 1) - G_final(y, x - 1);
      float right = R_final(y, x + 1) - G_final(y, x - 1);
      estimate_R  = G_final(y, x) + 0.5f * (left + right);
    } else if (color_idx == 3) {
      float up   = R_final(y - 1, x) - G_final(y - 1, x);
      float down = R_final(y + 1, x) - G_final(y + 1, x);
      estimate_R = G_final(y, x) + 0.5f * (up + down);
    } else if (color_idx == 2) {
      // At a B pixel: R is on diagonals
      float d1   = R_final(y - 1, x - 1) - G_final(y - 1, x - 1);
      float d2   = R_final(y - 1, x + 1) - G_final(y - 1, x + 1);
      float d3   = R_final(y + 1, x - 1) - G_final(y + 1, x - 1);
      float d4   = R_final(y + 1, x + 1) - G_final(y + 1, x + 1);
      estimate_R = G_final(y, x) + 0.25f * (d1 + d2 + d3 + d4);
    } else {
      float left  = R_final(y, x - 1) - G_final(y, x - 1);
      float right = R_final(y, x + 1) - G_final(y, x + 1);
      float up    = R_final(y - 1, x) - G_final(y - 1, x);
      float down  = R_final(y + 1, x) - G_final(y + 1, x);
      estimate_R  = G_final(y, x) + 0.25f * (left + right + up + down);
    }
    R_final(y, x) = estimate_R;
  }

  if (color_idx != 2) {
    float estimate_B = 0.0f;
    if (color_idx == 3) {
      float left  = B_final(y, x - 1) - G_final(y, x - 1);
      float right = B_final(y, x + 1) - G_final(y, x + 1);
      estimate_B  = G_final(y, x) + 0.5f * (left + right);
    } else if (color_idx == 1) {
      float up   = B_final(y - 1, x) - G_final(y - 1, x);
      float down = B_final(y + 1, x) - G_final(y + 1, x);
      estimate_B = G_final(y, x) + 0.5f * (up + down);
    } else if (color_idx == 0) {
      // At an R pixel: B is on diagonals
      float d1   = B_final(y - 1, x - 1) - G_final(y - 1, x - 1);
      float d2   = B_final(y - 1, x + 1) - G_final(y - 1, x + 1);
      float d3   = B_final(y + 1, x - 1) - G_final(y + 1, x - 1);
      float d4   = B_final(y + 1, x + 1) - G_final(y + 1, x + 1);
      estimate_B = G_final(y, x) + 0.25f * (d1 + d2 + d3 + d4);
    } else {
      float left  = B_final(y, x - 1) - G_final(y, x - 1);
      float right = B_final(y, x + 1) - G_final(y, x + 1);
      float up    = B_final(y - 1, x) - G_final(y - 1, x);
      float down  = B_final(y + 1, x) - G_final(y + 1, x);
      estimate_B  = G_final(y, x) + 0.25f * (left + right + up + down);
    }
    B_final(y, x) = estimate_B;
  }
}

void BayerRGGB2RGB_AHD(cv::cuda::GpuMat& image) {
  const dim3 threads_per_block(32, 32);
  const dim3 num_blocks((image.cols + threads_per_block.x - 1) / threads_per_block.x,
                        (image.rows + threads_per_block.y - 1) / threads_per_block.y);


  cv::cuda::GpuMat G_final = image.clone();
  cv::cuda::GpuMat R_final;
  R_final.create(image.size(), CV_32FC1);
  cv::cuda::GpuMat B_final;
  B_final.create(image.size(), CV_32FC1);

  G_FinalGeneration<<<num_blocks, threads_per_block>>>(image, G_final, R_final, B_final, image.cols,
                                                       image.rows);
  CUDA_CHECK(cudaGetLastError());
  CUDA_CHECK(cudaDeviceSynchronize());

  R_B_FinalGeneration<<<num_blocks, threads_per_block>>>(image, G_final, R_final, B_final,
                                                         image.cols, image.rows);
  CUDA_CHECK(cudaGetLastError());
  CUDA_CHECK(cudaDeviceSynchronize());

  MergeRGB(R_final, G_final, B_final, image);
}
};
};  // namespace alcedo
