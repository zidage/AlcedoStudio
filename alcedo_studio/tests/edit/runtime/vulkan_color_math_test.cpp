//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The shared color math of the Vulkan kernels against the OpenCL programs on the same GPU (plan
// phase VK3, decision B9). The Vulkan side runs the check kernels of color_math_check.slang; the
// OpenCL side runs the same functions from the product sources color_encoding_math.h and oklab.cl.
// Each test runs with the validation layer. Set ALCEDO_VULKAN_DEVICE (for example "Iris") to
// select the device; the OpenCL device follows it.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../../color/color_encoding_test_support.hpp"
#include "../../vulkan/vulkan_opencl_comparison.hpp"
#include "color/color_encoding_math.h"
#include "edit/runtime/vulkan/vulkan_kernel_dispatch.hpp"
#include "vulkan/vulkan_color_math_check_kernels.hpp"
#include "vulkan_workspace_test_support.hpp"

namespace alcedo {
namespace {

namespace check = vulkan::color_math_check_kernels;
using vulkan_opencl_comparison::DifferenceRule;
using vulkan_opencl_comparison::ExpectEqualWithinTolerance;
using vulkan_opencl_comparison::OpenClTestProgram;

// The ACEScc and OkLab functions: the tolerance of ColorEncodingOpenCl
// (color_encoding_opencl_test.cpp), ScaledDifference 1e-6.
constexpr double      kEncodingTolerance         = 1.0e-6;
// The transfer functions: the tolerance of a separate implementation of each curve,
// CurvesMatchOcioBuiltinsOn4096CodeValues (1e-5 relative, absolute below 0.01). The 1e-6 of
// ColorEncodingOpenCl holds because OpenCL and the host round each intermediate value in the same
// way. The Vulkan exp, exp2, log, and pow of math.slangh are about 1 ulp, and the shared formula
// amplifies one ulp of an intermediate value: 78.84 times in the ST 2084 encode (the exponent m2),
// 1.3e-6 in the LogC4 decode (exp2 of an argument near 19). Plan, phase VK3 record.
constexpr double      kTransferFunctionTolerance = 1.0e-5;
// OkLabToLinearAp1 is arithmetic only, and the compilers fuse other multiply-adds: about 2 ulp
// in l_, m_, s_, 3 times that after the cube, and the output matrix sums terms of about 3.35 times
// the result. The float32 limit is about 3.2e-6 of the result (plan, phase VK3 record).
constexpr double      kOkLabInverseTolerance     = 4.0e-6;

// The OpenCL forms of the two check kernels, after color_encoding_math.h and oklab.cl.
constexpr const char* kOpenClCheckKernels        = R"(
__kernel void check_transfer(__global const float* input, __global float* output, int count,
                             int mode) {
  const int i = (int)get_global_id(0);
  if (i >= count * CE_TF_COUNT) return;
  const int   tf    = i / count;
  const float value = input[i % count];
  output[i]         = mode != 0 ? CeEncode(tf, value) : CeDecode(tf, value);
}

__kernel void check_color(__global const float* input, __global float* output, int count,
                          int mode) {
  const int i = (int)get_global_id(0);
  if (i >= count) return;
  const float3 value = (float3)(input[4 * i], input[4 * i + 1], input[4 * i + 2]);
  float3       result;
  if (mode == 0) {
    result = AcesccToLinearAp1(value);
  } else if (mode == 1) {
    result = LinearAp1ToAcescc(value);
  } else if (mode == 2) {
    result = LinearAp1ToOkLab(value);
  } else {
    result = OkLabToLinearAp1(value);
  }
  output[4 * i]     = result.x;
  output[4 * i + 1] = result.y;
  output[4 * i + 2] = result.z;
  output[4 * i + 3] = 0.0f;
}
)";

class VulkanColorMathTest : public vulkan_workspace_test::VulkanWorkspaceFixture {
 protected:
  void SetUp() override {
    VulkanWorkspaceFixture::SetUp();
    const std::string reason = vulkan_opencl_comparison::PrepareOpenClOnTheVulkanDevice();
    ASSERT_TRUE(reason.empty()) << reason;
    opencl_ = std::make_unique<OpenClTestProgram>(
        std::vector<std::filesystem::path>{ALCEDO_COLOR_ENCODING_MATH_H_PATH,
                                           ALCEDO_OPENCL_OKLAB_CL_PATH},
        kOpenClCheckKernels);
  }

  void TearDown() override {
    opencl_.reset();
    VulkanWorkspaceFixture::TearDown();
  }

  /// Runs a check kernel on @p input and returns @p output_count values.
  template <class PushConstants>
  auto RunVulkan(const vulkan::VulkanKernelDescriptor& kernel, std::span<const float> input,
                 std::size_t output_count, PushConstants push, std::uint32_t work_items)
      -> std::vector<float> {
    VulkanRenderDevice device;
    auto&              backend         = device.Workspace().Device();
    auto&              command_context = device.CommandContext();
    auto               input_buffer    = backend.CreateBuffer(input.size_bytes());
    auto               output_buffer   = backend.CreateBuffer(output_count * sizeof(float));
    backend.UploadBufferRange(input_buffer, 0, std::as_bytes(input), command_context);
    push.input  = input_buffer.DeviceAddress();
    push.output = output_buffer.DeviceAddress();
    RecordKernelDispatch(backend, command_context, kernel, {}, PushConstantBytes(push),
                         {(work_items + check::kGroupSize - 1) / check::kGroupSize, 1, 1});
    std::vector<float> output(output_count);
    backend.DownloadBufferRange(output_buffer, 0, std::as_writable_bytes(std::span<float>(output)),
                                command_context);
    return output;
  }

  auto RunTransfer(const std::vector<float>& input, bool encode)
      -> std::pair<std::vector<float>, std::vector<float>> {
    const auto                   count = static_cast<std::uint32_t>(input.size());
    const auto                   total = static_cast<std::size_t>(count) * CE_TF_COUNT;
    check::TransferPushConstants push;
    push.count  = count;
    push.encode = encode ? 1U : 0U;
    auto vulkan = RunVulkan(check::TransferDescriptor(), input, total, push,
                            static_cast<std::uint32_t>(total));
    auto opencl = opencl_->Run("check_transfer", input, total, static_cast<int>(count),
                               encode ? 1 : 0, total);
    return {std::move(opencl), std::move(vulkan)};
  }

  auto RunColor(const std::vector<float>& rgba, check::ColorFunction function)
      -> std::pair<std::vector<float>, std::vector<float>> {
    const auto                count = static_cast<std::uint32_t>(rgba.size() / 4);
    check::ColorPushConstants push;
    push.count    = count;
    push.function = function;
    auto vulkan   = RunVulkan(check::ColorDescriptor(), rgba, rgba.size(), push, count);
    auto opencl   = opencl_->Run("check_color", rgba, rgba.size(), static_cast<int>(count),
                                 static_cast<int>(function), count);
    return {std::move(opencl), std::move(vulkan)};
  }

  /// Compares the values of one transfer function at a time, so a failure names the function.
  static void ExpectEachTransferFunctionEqual(const std::string& direction, std::size_t count,
                                              const std::vector<float>& opencl,
                                              const std::vector<float>& vulkan) {
    for (int tf = 0; tf < CE_TF_COUNT; ++tf) {
      const auto offset = static_cast<std::size_t>(tf) * count;
      ExpectEqualWithinTolerance(direction + " tf " + std::to_string(tf),
                                 std::span<const float>(opencl).subspan(offset, count),
                                 std::span<const float>(vulkan).subspan(offset, count),
                                 kTransferFunctionTolerance,
                                 DifferenceRule::RelativeAboveOneHundredth);
    }
  }

  std::unique_ptr<OpenClTestProgram> opencl_;
};

/// RGBA ACEScc colors of the grade input of VX1 (section 9.1, "photo"): ACEScc 0.25 to 0.65 with
/// channel spreads of +-0.06 (+-1 stop), a neutral color in every 16, and ties of the largest
/// channel in every 16 (offset 8).
auto PhotoAcesccColors() -> std::vector<float> {
  constexpr int      kCount = 4096;
  std::vector<float> rgba;
  rgba.reserve(kCount * 4);
  for (int i = 0; i < kCount; ++i) {
    const float base = 0.25F + 0.4F * static_cast<float>(i) / (kCount - 1);
    float       r    = base + 0.06F * std::sin(0.7F * static_cast<float>(i));
    float       g    = base + 0.06F * std::cos(1.3F * static_cast<float>(i));
    float       b    = base - 0.06F * std::sin(2.1F * static_cast<float>(i));
    if (i % 16 == 0) {
      r = g = b = base;
    } else if (i % 16 == 8) {
      g = r;
    }
    rgba.insert(rgba.end(), {r, g, b, 1.0F});
  }
  return rgba;
}

/// The ACEScc decode of @p acescc on the host (the linear AP1 input of the linear functions).
auto HostLinear(const std::vector<float>& acescc) -> std::vector<float> {
  std::vector<float> linear(acescc.size());
  for (std::size_t i = 0; i < acescc.size(); ++i) {
    linear[i] = i % 4 == 3 ? 1.0F : CeAcesccDecode(acescc[i]);
  }
  return linear;
}

TEST_F(VulkanColorMathTest, EveryTransferFunctionDecodesToTheOpenClValues) {
  const auto codes            = color_encoding_test::CodeValues();
  const auto [opencl, vulkan] = RunTransfer(codes, false);
  ExpectEachTransferFunctionEqual("decode", codes.size(), opencl, vulkan);
}

TEST_F(VulkanColorMathTest, EveryTransferFunctionEncodesToTheOpenClValues) {
  const auto linear           = color_encoding_test::LinearValues();
  const auto [opencl, vulkan] = RunTransfer(linear, true);
  ExpectEachTransferFunctionEqual("encode", linear.size(), opencl, vulkan);
}

TEST_F(VulkanColorMathTest, OkLabFunctionsGiveTheOpenClValues) {
  const auto acescc = PhotoAcesccColors();
  const auto linear = HostLinear(acescc);

  const auto [decoded_opencl, decoded_vulkan] =
      RunColor(acescc, check::ColorFunction::AcesccToLinearAp1);
  ExpectEqualWithinTolerance("AcesccToLinearAp1", decoded_opencl, decoded_vulkan,
                             kEncodingTolerance, DifferenceRule::ScaledAboveOne);

  const auto [encoded_opencl, encoded_vulkan] =
      RunColor(linear, check::ColorFunction::LinearAp1ToAcescc);
  ExpectEqualWithinTolerance("LinearAp1ToAcescc", encoded_opencl, encoded_vulkan,
                             kEncodingTolerance, DifferenceRule::ScaledAboveOne);

  const auto [lab_opencl, lab_vulkan] = RunColor(linear, check::ColorFunction::LinearAp1ToOkLab);
  ExpectEqualWithinTolerance("LinearAp1ToOkLab", lab_opencl, lab_vulkan, kEncodingTolerance,
                             DifferenceRule::ScaledAboveOne);

  // One OkLab input for both backends: the OpenCL OkLab of the colors.
  const auto [back_opencl, back_vulkan] =
      RunColor(lab_opencl, check::ColorFunction::OkLabToLinearAp1);
  ExpectEqualWithinTolerance("OkLabToLinearAp1", back_opencl, back_vulkan, kOkLabInverseTolerance,
                             DifferenceRule::ScaledAboveOne);
}

}  // namespace
}  // namespace alcedo
