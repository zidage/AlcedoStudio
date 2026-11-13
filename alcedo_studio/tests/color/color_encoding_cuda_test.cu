//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// CUDA evaluation of every curve of color/color_encoding_math.h against the host evaluation
// (lut_color_encoding_plan.md, Phase L1: equal within 1e-6).

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdio>
#include <vector>

#include "color/color_encoding_math.h"
#include "color_encoding_test_support.hpp"

namespace alcedo {
namespace {

/// out[tf * n + i] = decode (or encode) of in[i] with transfer tf.
__global__ void EvaluateCurves(const float* in, int n, int encode, float* out) {
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n * CE_TF_COUNT) return;
  const int   tf    = i / n;
  const float value = in[i % n];
  out[i]            = encode != 0 ? CeEncode(tf, value) : CeDecode(tf, value);
}

auto HasCudaDevice() -> bool {
  int count = 0;
  return cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

auto EvaluateOnDevice(const std::vector<float>& in, bool encode) -> std::vector<float> {
  const int          n     = static_cast<int>(in.size());
  const std::size_t  total = in.size() * CE_TF_COUNT;
  std::vector<float> out(total);
  float*             d_in  = nullptr;
  float*             d_out = nullptr;
  EXPECT_EQ(cudaMalloc(&d_in, in.size() * sizeof(float)), cudaSuccess);
  EXPECT_EQ(cudaMalloc(&d_out, total * sizeof(float)), cudaSuccess);
  EXPECT_EQ(cudaMemcpy(d_in, in.data(), in.size() * sizeof(float), cudaMemcpyHostToDevice),
            cudaSuccess);
  const int threads = 256;
  const int blocks  = static_cast<int>((total + threads - 1) / threads);
  EvaluateCurves<<<blocks, threads>>>(d_in, n, encode ? 1 : 0, d_out);
  EXPECT_EQ(cudaDeviceSynchronize(), cudaSuccess);
  EXPECT_EQ(cudaMemcpy(out.data(), d_out, total * sizeof(float), cudaMemcpyDeviceToHost),
            cudaSuccess);
  cudaFree(d_in);
  cudaFree(d_out);
  return out;
}

void ExpectDeviceMatchesHost(const std::vector<float>& in, bool encode) {
  const auto device = EvaluateOnDevice(in, encode);
  for (int tf = 0; tf < CE_TF_COUNT; ++tf) {
    SCOPED_TRACE(tf);
    float       worst    = 0.0f;
    std::size_t worst_at = 0;
    for (std::size_t i = 0; i < in.size(); ++i) {
      const float host       = encode ? CeEncode(tf, in[i]) : CeDecode(tf, in[i]);
      const float difference = color_encoding_test::ScaledDifference(
          device[static_cast<std::size_t>(tf) * in.size() + i], host);
      if (difference > worst) {
        worst    = difference;
        worst_at = i;
      }
    }
    std::printf("tf %d worst %.3e at input %.9g\n", tf, worst, in[worst_at]);
    EXPECT_LE(worst, 1e-6f);
  }
}

TEST(ColorEncodingCuda, DecodeOnCudaMatchesHostWithin1e6ForEveryCurve) {
  if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
  ExpectDeviceMatchesHost(color_encoding_test::CodeValues(), false);
}

TEST(ColorEncodingCuda, EncodeOnCudaMatchesHostWithin1e6ForEveryCurve) {
  if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
  ExpectDeviceMatchesHost(color_encoding_test::LinearValues(), true);
}

}  // namespace
}  // namespace alcedo
