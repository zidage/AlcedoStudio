//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "decoders/processor/operators/gpu/opencl_cvt_ref_space.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "image/opencl_image.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_program_library.hpp"

namespace alcedo {
namespace OpenCL {
namespace {

struct InverseCamMulParams {
  float    scale_r;
  float    scale_g;
  float    scale_b;
  float    scale_a;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
};

constexpr float kMinGain = 1e-6f;

void CheckOpenCl(cl_int err, const char* operation) {
  if (err != CL_SUCCESS) {
    throw std::runtime_error(std::string("OpenCL ApplyInverseCamMul: ") + operation +
                             " failed with error " + std::to_string(err) + ".");
  }
}

auto RoundUpToMultiple(uint32_t value, uint32_t multiple) -> uint32_t {
  return ((value + multiple - 1) / multiple) * multiple;
}

}  // namespace

void ApplyInverseCamMul(opencl::OpenClImage& img, const float* cam_mul) {
  if (img.Empty()) {
    return;
  }
  if (img.Type() != CV_32FC4) {
    throw std::runtime_error("OpenCL ApplyInverseCamMul: expected CV_32FC4 input.");
  }
  if (cam_mul == nullptr) {
    throw std::runtime_error("OpenCL ApplyInverseCamMul: cam_mul is null.");
  }

  auto& context = OpenClContext::Instance();
  if (!context.IsInitialized()) {
    context.Initialize();
  }

  cl_program program = OpenClProgramLibrary::Instance().GetProgram("raw_processor_cvt_ref_space");
  cl_int     err     = CL_SUCCESS;
  cl_kernel  kernel  = clCreateKernel(program, "apply_inverse_cam_mul_rgba32f", &err);
  CheckOpenCl(err, "clCreateKernel");

  const float g      = std::max(cam_mul[1], kMinGain);
  const InverseCamMulParams params{
      .scale_r = g / std::max(cam_mul[0], kMinGain),
      .scale_g = 1.0f,
      .scale_b = g / std::max(cam_mul[2], kMinGain),
      .scale_a = 1.0f,
      .width   = static_cast<uint32_t>(img.Width()),
      .height  = static_cast<uint32_t>(img.Height()),
      .stride  = static_cast<uint32_t>(img.RowBytes() / (sizeof(float) * 4)),
  };

  cl_mem buffer = img.Buffer();
  err           = clSetKernelArg(kernel, 0, sizeof(cl_mem), &buffer);
  CheckOpenCl(err, "clSetKernelArg(0)");
  err = clSetKernelArg(kernel, 1, sizeof(InverseCamMulParams), &params);
  CheckOpenCl(err, "clSetKernelArg(1)");

  const size_t local_size[2]  = {16, 16};
  const size_t global_size[2] = {
      RoundUpToMultiple(params.width, 16),
      RoundUpToMultiple(params.height, 16),
  };

  err = clEnqueueNDRangeKernel(context.Queue(), kernel, 2, nullptr, global_size, local_size, 0,
                               nullptr, nullptr);
  CheckOpenCl(err, "clEnqueueNDRangeKernel");
  err = clFinish(context.Queue());
  CheckOpenCl(err, "clFinish");

  clReleaseKernel(kernel);
}

}  // namespace OpenCL
}  // namespace alcedo

#endif
