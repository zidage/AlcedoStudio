//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Comparison of Vulkan results with OpenCL results on the same GPU (plan decision B9, phase VK3).
// A Vulkan pass is complete when a test runs one input on Vulkan and on OpenCL and the difference
// is in the tolerance of the matching OpenCL test. Each comparison test of VK3 and later uses:
//
// - PrepareOpenClOnTheVulkanDevice: the OpenCL runtime on the GPU of the Vulkan context;
// - ExpectEqualWithinTolerance: the largest difference, its index, both values, and the count of
//   values above the tolerance, for a stated difference rule;
// - OpenClTestProgram: an OpenCL program of product source files and one test kernel.

#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "opencl/opencl_context.hpp"
#include "opencl/opencl_runtime.hpp"
#include "vulkan/vulkan_context.hpp"

namespace alcedo::vulkan_opencl_comparison {

inline auto Lowercase(std::string text) -> std::string {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

/**
 * @brief Initializes the OpenCL runtime of the process on the GPU of the Vulkan context.
 *
 * The preferred OpenCL device is the name of the Vulkan device, so `ALCEDO_VULKAN_DEVICE` selects
 * the device of both backends. The OpenCL context is a process singleton: when it exists from an
 * earlier test, its device must have the same name.
 *
 * @pre `VulkanContext` exists.
 * @return An empty text on success, else the reason (no OpenCL device, or another device).
 */
inline auto PrepareOpenClOnTheVulkanDevice() -> std::string {
  const std::string vulkan_name = vulkan::VulkanContext::Instance().Capabilities().name;
  auto&             opencl      = OpenClContext::Instance();
  if (!opencl.IsInitialized()) {
    OpenClInitializationOptions options;
    options.preferred_device = vulkan_name;
    if (!TryInitializeOpenClRuntime(options)) {
      return "OpenCL runtime is unavailable: " + opencl.LastInitializationError();
    }
  }
  const std::string opencl_name = opencl.Capabilities().name;
  if (Lowercase(opencl_name) != Lowercase(vulkan_name)) {
    return "the OpenCL device '" + opencl_name + "' is not the Vulkan device '" + vulkan_name +
           "'; a comparison needs one GPU";
  }
  return {};
}

/// How a difference of two values is measured.
enum class DifferenceRule {
  Absolute,        ///< |vulkan - opencl|
  ScaledAboveOne,  ///< |vulkan - opencl| / max(1, |opencl|): `ScaledDifference` of the encoding
                   ///< tests
  /// |vulkan - opencl| / max(0.01, |opencl|): the rule of `CurvesMatchOcioBuiltinsOn4096CodeValues`
  RelativeAboveOneHundredth,
};

/// The result of a comparison of two value lists.
struct ValueComparison {
  double      largest = 0.0;  ///< largest difference by the rule
  std::size_t index   = 0;    ///< index of the largest difference
  float       opencl  = 0.0F;
  float       vulkan  = 0.0F;
  std::size_t above   = 0;  ///< values with a difference above the tolerance
};

inline auto Difference(float opencl, float vulkan, DifferenceRule rule) -> double {
  if (std::isnan(opencl) || std::isnan(vulkan)) {
    return std::isnan(opencl) && std::isnan(vulkan) ? 0.0 : INFINITY;
  }
  if (opencl == vulkan) {
    return 0.0;  // also equal infinities
  }
  const double difference = std::abs(static_cast<double>(vulkan) - static_cast<double>(opencl));
  const double magnitude  = std::abs(static_cast<double>(opencl));
  switch (rule) {
    case DifferenceRule::Absolute:
      return difference;
    case DifferenceRule::ScaledAboveOne:
      return difference / std::max(1.0, magnitude);
    case DifferenceRule::RelativeAboveOneHundredth:
      return difference / std::max(0.01, magnitude);
  }
  return difference;
}

/// Compares @p vulkan with @p opencl value by value. @pre Both have the same size.
inline auto CompareValues(std::span<const float> opencl, std::span<const float> vulkan,
                          double tolerance, DifferenceRule rule) -> ValueComparison {
  if (opencl.size() != vulkan.size()) {
    throw std::invalid_argument("CompareValues: " + std::to_string(opencl.size()) +
                                " OpenCL values and " + std::to_string(vulkan.size()) +
                                " Vulkan values");
  }
  ValueComparison result;
  for (std::size_t i = 0; i < opencl.size(); ++i) {
    const double difference = Difference(opencl[i], vulkan[i], rule);
    if (difference > tolerance) {
      ++result.above;
    }
    if (difference > result.largest || (i == 0 && difference == 0.0)) {
      result.largest = difference;
      result.index   = i;
      result.opencl  = opencl[i];
      result.vulkan  = vulkan[i];
    }
  }
  return result;
}

/**
 * @brief Expects that no value of @p vulkan differs from @p opencl by more than @p tolerance.
 *
 * Prints one line with the two device names and the largest difference, so the record of a phase
 * can give the measured difference of each case.
 */
inline void ExpectEqualWithinTolerance(const std::string& label, std::span<const float> opencl,
                                       std::span<const float> vulkan, double tolerance,
                                       DifferenceRule rule) {
  ASSERT_EQ(opencl.size(), vulkan.size()) << label;
  const auto result = CompareValues(opencl, vulkan, tolerance, rule);
  std::printf(
      "[%s] Vulkan '%s' against OpenCL '%s': largest difference %.3e at index %zu (OpenCL %.9g, "
      "Vulkan %.9g); %zu of %zu values above %.1e\n",
      label.c_str(), vulkan::VulkanContext::Instance().Capabilities().name.c_str(),
      OpenClContext::Instance().Capabilities().name.c_str(), result.largest, result.index,
      result.opencl, result.vulkan, result.above, opencl.size(), tolerance);
  EXPECT_EQ(result.above, 0U) << label << ": largest difference " << result.largest << " at index "
                              << result.index << " (OpenCL " << result.opencl << ", Vulkan "
                              << result.vulkan << "), tolerance " << tolerance;
}

inline auto ReadSourceFile(const std::filesystem::path& path) -> std::string {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot read the OpenCL source " + path.string());
  }
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/**
 * @brief An OpenCL program of product source files and one test kernel, on the device of
 * `OpenClContext` (the same source order as the product program list of
 * `opencl_gpu_dag_programs.cpp`).
 *
 * The test kernels have the arguments `(__global const float* input, __global float* output,
 * int count, int mode)`; @ref Run gives them and reads the output.
 */
class OpenClTestProgram {
 public:
  /// @throws std::runtime_error With the build log when the program does not build.
  OpenClTestProgram(const std::vector<std::filesystem::path>& source_files,
                    const std::string&                        kernel_source) {
    for (const auto& path : source_files) {
      sources_.push_back(ReadSourceFile(path));
    }
    sources_.push_back(kernel_source);
    std::vector<const char*> texts;
    for (const auto& text : sources_) {
      texts.push_back(text.c_str());
    }
    auto&  context = OpenClContext::Instance();
    cl_int error   = CL_SUCCESS;
    program_ = clCreateProgramWithSource(context.Context(), static_cast<cl_uint>(texts.size()),
                                         texts.data(), nullptr, &error);
    Check(error, "clCreateProgramWithSource");
    cl_device_id device = context.Device();
    if (clBuildProgram(program_, 1, &device, "-cl-std=CL1.2", nullptr, nullptr) != CL_SUCCESS) {
      std::size_t size = 0;
      clGetProgramBuildInfo(program_, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &size);
      std::string log(size, '\0');
      clGetProgramBuildInfo(program_, device, CL_PROGRAM_BUILD_LOG, size, log.data(), nullptr);
      clReleaseProgram(program_);
      throw std::runtime_error("OpenCL test program does not build:\n" + log);
    }
  }
  ~OpenClTestProgram() { clReleaseProgram(program_); }
  OpenClTestProgram(const OpenClTestProgram&)                    = delete;
  auto operator=(const OpenClTestProgram&) -> OpenClTestProgram& = delete;

  /// Runs @p kernel over @p work_items items and returns @p output_count values.
  auto Run(const char* kernel, std::span<const float> input, std::size_t output_count, int count,
           int mode, std::size_t work_items) -> std::vector<float> {
    auto&     context = OpenClContext::Instance();
    cl_int    error   = CL_SUCCESS;
    cl_kernel handle  = clCreateKernel(program_, kernel, &error);
    Check(error, "clCreateKernel");
    cl_mem input_buffer =
        clCreateBuffer(context.Context(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                       input.size_bytes(), const_cast<float*>(input.data()), &error);
    Check(error, "clCreateBuffer");
    cl_mem output_buffer = clCreateBuffer(context.Context(), CL_MEM_WRITE_ONLY,
                                          output_count * sizeof(float), nullptr, &error);
    Check(error, "clCreateBuffer");
    clSetKernelArg(handle, 0, sizeof(cl_mem), &input_buffer);
    clSetKernelArg(handle, 1, sizeof(cl_mem), &output_buffer);
    clSetKernelArg(handle, 2, sizeof(int), &count);
    clSetKernelArg(handle, 3, sizeof(int), &mode);
    std::vector<float> output(output_count);
    cl_command_queue   queue = context.Queue();
    Check(clEnqueueNDRangeKernel(queue, handle, 1, nullptr, &work_items, nullptr, 0, nullptr,
                                 nullptr),
          "clEnqueueNDRangeKernel");
    Check(clEnqueueReadBuffer(queue, output_buffer, CL_TRUE, 0, output_count * sizeof(float),
                              output.data(), 0, nullptr, nullptr),
          "clEnqueueReadBuffer");
    clReleaseMemObject(input_buffer);
    clReleaseMemObject(output_buffer);
    clReleaseKernel(handle);
    return output;
  }

 private:
  static void Check(cl_int error, const char* operation) {
    if (error != CL_SUCCESS) {
      throw std::runtime_error(std::string("OpenCL test program: ") + operation +
                               " failed with error " + std::to_string(error));
    }
  }

  std::vector<std::string> sources_;
  cl_program               program_ = nullptr;
};

}  // namespace alcedo::vulkan_opencl_comparison
