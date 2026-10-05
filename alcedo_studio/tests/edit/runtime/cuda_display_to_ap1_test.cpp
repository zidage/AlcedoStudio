//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// CUDA DisplayToAp1 kernels against OpenColorIO 2.5.1 and the host evaluation of the shared
// per-pixel code (raster_image_input_plan.md, section 5.7).

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <span>
#include <vector>

#include "aces2_inverse_ocio_reference.hpp"
#include "edit/runtime/cuda/cuda_display_to_ap1_pass.hpp"
#include "edit/runtime/display_to_ap1_math.h"
#include "edit/runtime/drt/aces2_inverse_runtime.hpp"
#include "image/raster_color_description.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

class CudaDisplayToAp1Test : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
  }
};

/// Run the kernel over interleaved RGB and return interleaved RGB.
auto RunOnCuda(std::span<const float> packed, const std::vector<float>& rgb,
               DisplayToAp1Output output_kind, CudaDisplayToAp1Parameters& parameters)
    -> std::vector<float> {
  const auto          pixels = static_cast<std::uint32_t>(rgb.size() / 3);
  std::vector<float4> host(pixels);
  for (std::uint32_t i = 0; i < pixels; ++i) {
    host[i] = make_float4(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2], 0.25f);
  }
  float4* input  = nullptr;
  float4* output = nullptr;
  EXPECT_EQ(::cudaMalloc(reinterpret_cast<void**>(&input), pixels * sizeof(float4)), cudaSuccess);
  EXPECT_EQ(::cudaMalloc(reinterpret_cast<void**>(&output), pixels * sizeof(float4)), cudaSuccess);
  EXPECT_EQ(::cudaMemcpy(input, host.data(), pixels * sizeof(float4), cudaMemcpyHostToDevice),
            cudaSuccess);
  const float* device_params = parameters.Upload(packed, nullptr);
  LaunchCudaDisplayToAp1(input, output, pixels, device_params, output_kind, nullptr);
  EXPECT_EQ(::cudaMemcpy(host.data(), output, pixels * sizeof(float4), cudaMemcpyDeviceToHost),
            cudaSuccess);
  ::cudaFree(input);
  ::cudaFree(output);
  std::vector<float> out(rgb.size());
  for (std::uint32_t i = 0; i < pixels; ++i) {
    out[i * 3]     = host[i].x;
    out[i * 3 + 1] = host[i].y;
    out[i * 3 + 2] = host[i].z;
    EXPECT_EQ(host[i].w, 0.25f);
  }
  return out;
}

TEST_F(CudaDisplayToAp1Test, Aces2InverseMatchesOcioCpuProcessor) {
  CudaDisplayToAp1Parameters parameters;
  for (const auto& c : aces2_inverse_test::InverseCases()) {
    SCOPED_TRACE(c.name_);
    const auto runtime = ResolveAces2InverseRuntime(c.primaries_, c.peak_nits_);
    const auto grid    = aces2_inverse_test::DisplayGrid(c.peak_nits_);
    aces2_inverse_test::ExpectMatchesOcio(
        c, grid, RunOnCuda(runtime->packed_, grid, DisplayToAp1Output::LinearAp0, parameters));
  }
}

TEST_F(CudaDisplayToAp1Test, Aces2InverseReturnsBlackForBlackAndNeutralForSourceWhite) {
  CudaDisplayToAp1Parameters parameters;
  for (const auto& c : aces2_inverse_test::InverseCases()) {
    SCOPED_TRACE(c.name_);
    const auto         runtime = ResolveAces2InverseRuntime(c.primaries_, c.peak_nits_);
    const float        peak    = c.peak_nits_ / 100.0f;
    std::vector<float> rgb     = {0.0f, 0.0f, 0.0f};
    for (const float fraction : {0.0005f, 0.05f, 0.5f, 0.9f}) {
      rgb.insert(rgb.end(), 3, fraction * peak);
    }
    const auto out = RunOnCuda(runtime->packed_, rgb, DisplayToAp1Output::LinearAp0, parameters);
    EXPECT_EQ(out[0], 0.0f);
    EXPECT_EQ(out[1], 0.0f);
    EXPECT_EQ(out[2], 0.0f);
    for (std::size_t p = 1; p < out.size() / 3; ++p) {
      const auto [lo, hi] = std::minmax({out[p * 3], out[p * 3 + 1], out[p * 3 + 2]});
      ASSERT_GT(lo, 0.0f);
      EXPECT_LT(hi / lo - 1.0f, 1e-4f) << "level index " << p;
    }
  }
}

TEST_F(CudaDisplayToAp1Test, AcesccOutputMatchesHostEvaluationForBothBranches) {
  CudaDisplayToAp1Parameters parameters;
  const auto         display = ResolveAces2InverseRuntime(kRasterPrimariesDisplayP3, 100.0f);
  const auto         scene   = PackSceneLinearToAp1(kRasterPrimariesAp0);
  std::vector<float> rgb;
  for (int i = 0; i < 64; ++i) {
    rgb.push_back(static_cast<float>(i % 4) / 3.0f);
    rgb.push_back(static_cast<float>((i / 4) % 4) / 3.0f);
    rgb.push_back(static_cast<float>(i / 16) / 3.0f);
  }
  for (const std::span<const float> packed :
       {std::span<const float>(display->packed_), std::span<const float>(scene)}) {
    const auto gpu = RunOnCuda(packed, rgb, DisplayToAp1Output::AcesccAp1, parameters);
    // 2e-4 in ACEScc is 0.25 percent in linear light: the device and host pow differ in the
    // last bits.
    for (std::size_t p = 0; p < rgb.size() / 3; ++p) {
      const auto host =
          D2aSourceToAcesccAp1(D2aMake3(rgb[p * 3], rgb[p * 3 + 1], rgb[p * 3 + 2]), packed.data());
      EXPECT_NEAR(gpu[p * 3], host.x, 2e-4f) << "pixel " << p;
      EXPECT_NEAR(gpu[p * 3 + 1], host.y, 2e-4f) << "pixel " << p;
      EXPECT_NEAR(gpu[p * 3 + 2], host.z, 2e-4f) << "pixel " << p;
    }
  }
}

TEST_F(CudaDisplayToAp1Test, ParameterBlockIsUploadedOnlyWhenItChanges) {
  CudaDisplayToAp1Parameters parameters;
  const auto                 a = ResolveAces2InverseRuntime(kRasterPrimariesRec709, 100.0f);
  const auto                 b = ResolveAces2InverseRuntime(kRasterPrimariesRec2020, 100.0f);
  (void)parameters.Upload(a->packed_, nullptr);
  (void)parameters.Upload(a->packed_, nullptr);
  EXPECT_EQ(parameters.UploadCount(), 1u);
  (void)parameters.Upload(b->packed_, nullptr);
  EXPECT_EQ(parameters.UploadCount(), 2u);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
}

TEST_F(CudaDisplayToAp1Test, InverseKernelTimeIsMeasuredAtUhdExtent) {
  // Section 5.4 records the pass time; the ratio to the forward DRT pass is measured with the
  // pass statistics once the pass runs in the develop graph.
  constexpr std::uint32_t    kPixels = 3840u * 2160u;
  CudaDisplayToAp1Parameters parameters;
  const auto runtime = ResolveAces2InverseRuntime(kRasterPrimariesDisplayP3, 100.0f);
  float4*    input   = nullptr;
  float4*    output  = nullptr;
  ASSERT_EQ(::cudaMalloc(reinterpret_cast<void**>(&input), kPixels * sizeof(float4)), cudaSuccess);
  ASSERT_EQ(::cudaMalloc(reinterpret_cast<void**>(&output), kPixels * sizeof(float4)), cudaSuccess);
  std::vector<float4> host(kPixels);
  for (std::uint32_t i = 0; i < kPixels; ++i) {
    host[i] = make_float4(static_cast<float>(i % 3840) / 3839.0f,
                          static_cast<float>(i / 3840) / 2159.0f, 0.4f, 1.0f);
  }
  ASSERT_EQ(::cudaMemcpy(input, host.data(), kPixels * sizeof(float4), cudaMemcpyHostToDevice),
            cudaSuccess);
  const float* params = parameters.Upload(runtime->packed_, nullptr);
  LaunchCudaDisplayToAp1(input, output, kPixels, params, DisplayToAp1Output::AcesccAp1, nullptr);
  cudaEvent_t start{}, stop{};
  ::cudaEventCreate(&start);
  ::cudaEventCreate(&stop);
  ::cudaEventRecord(start);
  constexpr int kRuns = 10;
  for (int i = 0; i < kRuns; ++i) {
    LaunchCudaDisplayToAp1(input, output, kPixels, params, DisplayToAp1Output::AcesccAp1, nullptr);
  }
  ::cudaEventRecord(stop);
  ASSERT_EQ(::cudaEventSynchronize(stop), cudaSuccess);
  float ms = 0.0f;
  ::cudaEventElapsedTime(&ms, start, stop);
  std::printf("CUDA DisplayToAp1 3840x2160: %.3f ms per pass\n", ms / kRuns);
  RecordProperty("display_to_ap1_uhd_ms", std::to_string(ms / kRuns));
  ::cudaEventDestroy(start);
  ::cudaEventDestroy(stop);
  ::cudaFree(input);
  ::cudaFree(output);
  EXPECT_GT(ms, 0.0f);
}

}  // namespace
}  // namespace alcedo
