//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// OpenCL C evaluation of every curve of color/color_encoding_math.h against the host evaluation
// (lut_color_encoding_plan.md, Phase L1: equal within 1e-6). The program is the header followed
// by one kernel, as the product programs list it (opencl_gpu_dag_programs.cpp).

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "color/color_encoding_math.h"
#include "color_encoding_test_support.hpp"

namespace alcedo {
namespace {

constexpr const char* kKernelSource = R"(
__kernel void evaluate_curves(__global const float* in, int n, int encode, __global float* out) {
  const int i = (int)get_global_id(0);
  if (i >= n * CE_TF_COUNT) return;
  const int   tf    = i / n;
  const float value = in[i % n];
  out[i]            = encode != 0 ? CeEncode(tf, value) : CeDecode(tf, value);
}
)";

/// OpenCL objects of the first GPU device, or empty when there is none.
class OpenClCurveProgram {
 public:
  OpenClCurveProgram() {
    cl_uint platform_count = 0;
    if (clGetPlatformIDs(0, nullptr, &platform_count) != CL_SUCCESS || platform_count == 0) return;
    std::vector<cl_platform_id> platforms(platform_count);
    clGetPlatformIDs(platform_count, platforms.data(), nullptr);
    for (auto platform : platforms) {
      if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device_, nullptr) == CL_SUCCESS) break;
      device_ = nullptr;
    }
    if (device_ == nullptr) return;
    cl_int error = CL_SUCCESS;
    context_     = clCreateContext(nullptr, 1, &device_, nullptr, nullptr, &error);
    queue_       = clCreateCommandQueue(context_, device_, 0, &error);
    std::ifstream header_file(ALCEDO_COLOR_ENCODING_MATH_SOURCE_PATH);
    header_                = std::string(std::istreambuf_iterator<char>(header_file), {});
    const char* sources[2] = {header_.c_str(), kKernelSource};
    program_               = clCreateProgramWithSource(context_, 2, sources, nullptr, &error);
    build_error_ = clBuildProgram(program_, 1, &device_, "-cl-std=CL1.2", nullptr, nullptr);
    if (build_error_ != CL_SUCCESS) {
      std::size_t size = 0;
      clGetProgramBuildInfo(program_, device_, CL_PROGRAM_BUILD_LOG, 0, nullptr, &size);
      build_log_.resize(size);
      clGetProgramBuildInfo(program_, device_, CL_PROGRAM_BUILD_LOG, size, build_log_.data(),
                            nullptr);
      return;
    }
    kernel_ = clCreateKernel(program_, "evaluate_curves", &error);
  }

  ~OpenClCurveProgram() {
    if (kernel_ != nullptr) clReleaseKernel(kernel_);
    if (program_ != nullptr) clReleaseProgram(program_);
    if (queue_ != nullptr) clReleaseCommandQueue(queue_);
    if (context_ != nullptr) clReleaseContext(context_);
  }

  OpenClCurveProgram(const OpenClCurveProgram&)            = delete;
  OpenClCurveProgram& operator=(const OpenClCurveProgram&) = delete;

  [[nodiscard]] auto  HasDevice() const -> bool { return device_ != nullptr; }
  [[nodiscard]] auto  BuildError() const -> cl_int { return build_error_; }
  [[nodiscard]] auto  BuildLog() const -> const std::string& { return build_log_; }

  auto                Evaluate(const std::vector<float>& in, bool encode) -> std::vector<float> {
    const cl_int       n     = static_cast<cl_int>(in.size());
    const std::size_t  total = in.size() * CE_TF_COUNT;
    std::vector<float> out(total);
    cl_int             error = CL_SUCCESS;
    cl_mem             in_buffer =
        clCreateBuffer(context_, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, in.size() * sizeof(float),
                                      const_cast<float*>(in.data()), &error);
    EXPECT_EQ(error, CL_SUCCESS);
    cl_mem out_buffer =
        clCreateBuffer(context_, CL_MEM_WRITE_ONLY, total * sizeof(float), nullptr, &error);
    EXPECT_EQ(error, CL_SUCCESS);
    const cl_int mode = encode ? 1 : 0;
    clSetKernelArg(kernel_, 0, sizeof(cl_mem), &in_buffer);
    clSetKernelArg(kernel_, 1, sizeof(cl_int), &n);
    clSetKernelArg(kernel_, 2, sizeof(cl_int), &mode);
    clSetKernelArg(kernel_, 3, sizeof(cl_mem), &out_buffer);
    EXPECT_EQ(
        clEnqueueNDRangeKernel(queue_, kernel_, 1, nullptr, &total, nullptr, 0, nullptr, nullptr),
        CL_SUCCESS);
    EXPECT_EQ(clEnqueueReadBuffer(queue_, out_buffer, CL_TRUE, 0, total * sizeof(float), out.data(),
                                                 0, nullptr, nullptr),
                             CL_SUCCESS);
    clReleaseMemObject(in_buffer);
    clReleaseMemObject(out_buffer);
    return out;
  }

 private:
  cl_device_id     device_      = nullptr;
  cl_context       context_     = nullptr;
  cl_command_queue queue_       = nullptr;
  cl_program       program_     = nullptr;
  cl_kernel        kernel_      = nullptr;
  cl_int           build_error_ = CL_SUCCESS;
  std::string      header_;
  std::string      build_log_;
};

void ExpectDeviceMatchesHost(OpenClCurveProgram& program, const std::vector<float>& in,
                             bool encode) {
  const auto device = program.Evaluate(in, encode);
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

TEST(ColorEncodingOpenCl, DecodeOnOpenClMatchesHostWithin1e6ForEveryCurve) {
  OpenClCurveProgram program;
  if (!program.HasDevice()) GTEST_SKIP() << "No OpenCL GPU device available.";
  ASSERT_EQ(program.BuildError(), CL_SUCCESS) << program.BuildLog();
  ExpectDeviceMatchesHost(program, color_encoding_test::CodeValues(), false);
}

TEST(ColorEncodingOpenCl, EncodeOnOpenClMatchesHostWithin1e6ForEveryCurve) {
  OpenClCurveProgram program;
  if (!program.HasDevice()) GTEST_SKIP() << "No OpenCL GPU device available.";
  ASSERT_EQ(program.BuildError(), CL_SUCCESS) << program.BuildLog();
  ExpectDeviceMatchesHost(program, color_encoding_test::LinearValues(), true);
}

}  // namespace
}  // namespace alcedo
