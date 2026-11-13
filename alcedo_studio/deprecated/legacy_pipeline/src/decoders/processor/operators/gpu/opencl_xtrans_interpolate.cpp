//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "decoders/processor/operators/gpu/opencl_xtrans_interpolate.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

#include "image/opencl_image.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_program_library.hpp"

namespace alcedo {
namespace OpenCL {
namespace {

struct XTransParams {
  uint32_t width;
  uint32_t height;
  uint32_t tile_width;
  uint32_t tile_height;
  uint32_t passes;
  uint32_t green_radius;
  uint32_t rb_radius;
  uint32_t rgb_fc[36];
};

void CheckOpenCl(cl_int err, const char* operation) {
  if (err != CL_SUCCESS) {
    throw std::runtime_error(std::string("OpenCL X-Trans interpolate: ") + operation +
                             " failed with error " + std::to_string(err) + ".");
  }
}

auto RoundUpToMultiple(uint32_t value, uint32_t multiple) -> uint32_t {
  return ((value + multiple - 1) / multiple) * multiple;
}

void DispatchKernel(cl_kernel kernel, uint32_t width, uint32_t height) {
  auto&    context = OpenClContext::Instance();
  uint32_t local_x = 16;
  uint32_t local_y = 16;
  size_t   global[2] = {RoundUpToMultiple(width, local_x), RoundUpToMultiple(height, local_y)};
  size_t   local[2] = {local_x, local_y};

  cl_int err = clEnqueueNDRangeKernel(context.Queue(), kernel, 2, nullptr, global, local, 0,
                                      nullptr, nullptr);
  CheckOpenCl(err, "clEnqueueNDRangeKernel");
}

}  // namespace

void XTransToRGB_Ref(opencl::OpenClImage& image, const XTransPattern6x6& pattern, int passes) {
  if (image.Empty()) {
    throw std::runtime_error("OpenCL X-Trans interpolate: input image is empty.");
  }
  if (image.Type() != CV_32FC1) {
    throw std::runtime_error("OpenCL X-Trans interpolate: expected CV_32FC1 raw input.");
  }

  const uint32_t width  = static_cast<uint32_t>(image.Width());
  const uint32_t height = static_cast<uint32_t>(image.Height());
  if (width == 0 || height == 0) {
    return;
  }

  opencl::OpenClImage green;
  opencl::OpenClImage output;
  green.Create(static_cast<int>(width), static_cast<int>(height), CV_32FC1);
  output.Create(static_cast<int>(width), static_cast<int>(height), CV_32FC4);

  XTransParams params = {};
  params.width        = width;
  params.height       = height;
  params.tile_width   = 6;
  params.tile_height  = 6;
  params.passes       = static_cast<uint32_t>(std::max(passes, 1));
  params.green_radius = 3;
  params.rb_radius    = params.passes > 1 ? 4U : 3U;
  for (int i = 0; i < 36; ++i) {
    params.rgb_fc[i] = static_cast<uint32_t>(pattern.rgb_fc[i]);
  }

  auto& context = OpenClContext::Instance();
  if (!context.IsInitialized()) {
    context.Initialize();
  }

  cl_program program = OpenClProgramLibrary::Instance().GetProgram("raw_processor_xtrans");
  cl_int     err     = CL_SUCCESS;

  cl_mem raw_buf    = image.Buffer();
  cl_mem green_buf  = green.Buffer();
  cl_mem output_buf = output.Buffer();

  cl_kernel green_kernel = clCreateKernel(program, "xtrans_green", &err);
  CheckOpenCl(err, "clCreateKernel(xtrans_green)");
  err = clSetKernelArg(green_kernel, 0, sizeof(cl_mem), &raw_buf);
  CheckOpenCl(err, "clSetKernelArg(green,0)");
  err = clSetKernelArg(green_kernel, 1, sizeof(cl_mem), &green_buf);
  CheckOpenCl(err, "clSetKernelArg(green,1)");
  err = clSetKernelArg(green_kernel, 2, sizeof(XTransParams), &params);
  CheckOpenCl(err, "clSetKernelArg(green,2)");
  const cl_uint zero = 0;
  err = clSetKernelArg(green_kernel, 3, sizeof(cl_uint), &zero);
  CheckOpenCl(err, "clSetKernelArg(green,3)");
  err = clSetKernelArg(green_kernel, 4, sizeof(cl_uint), &zero);
  CheckOpenCl(err, "clSetKernelArg(green,4)");
  DispatchKernel(green_kernel, width, height);
  clReleaseKernel(green_kernel);

  cl_kernel rgba_kernel = clCreateKernel(program, "xtrans_rgba", &err);
  CheckOpenCl(err, "clCreateKernel(xtrans_rgba)");
  err = clSetKernelArg(rgba_kernel, 0, sizeof(cl_mem), &raw_buf);
  CheckOpenCl(err, "clSetKernelArg(rgba,0)");
  err = clSetKernelArg(rgba_kernel, 1, sizeof(cl_mem), &green_buf);
  CheckOpenCl(err, "clSetKernelArg(rgba,1)");
  err = clSetKernelArg(rgba_kernel, 2, sizeof(cl_mem), &output_buf);
  CheckOpenCl(err, "clSetKernelArg(rgba,2)");
  err = clSetKernelArg(rgba_kernel, 3, sizeof(XTransParams), &params);
  CheckOpenCl(err, "clSetKernelArg(rgba,3)");
  err = clSetKernelArg(rgba_kernel, 4, sizeof(cl_uint), &zero);
  CheckOpenCl(err, "clSetKernelArg(rgba,4)");
  err = clSetKernelArg(rgba_kernel, 5, sizeof(cl_uint), &zero);
  CheckOpenCl(err, "clSetKernelArg(rgba,5)");
  err = clSetKernelArg(rgba_kernel, 6, sizeof(cl_uint), &zero);
  CheckOpenCl(err, "clSetKernelArg(rgba,6)");
  DispatchKernel(rgba_kernel, width, height);
  clReleaseKernel(rgba_kernel);

  err = clFinish(context.Queue());
  CheckOpenCl(err, "clFinish");

  image = std::move(output);
}

}  // namespace OpenCL
}  // namespace alcedo

#endif
