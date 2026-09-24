//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "decoders/processor/operators/gpu/opencl_debayer_rcd.hpp"

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

struct SinglePlaneParams {
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint32_t rgb_fc[4];
};

struct MergeParams {
  uint32_t width;
  uint32_t height;
  uint32_t plane_stride;
  uint32_t rgba_stride;
};

void CheckOpenCl(cl_int err, const char* operation) {
  if (err != CL_SUCCESS) {
    throw std::runtime_error(std::string("OpenCL Debayer RCD: ") + operation +
                             " failed with error " + std::to_string(err) + ".");
  }
}

auto RoundUpToMultiple(uint32_t value, uint32_t multiple) -> uint32_t {
  return ((value + multiple - 1) / multiple) * multiple;
}

void DispatchKernel(cl_kernel kernel, uint32_t width, uint32_t height) {
  auto&    context   = OpenClContext::Instance();
  uint32_t local_x   = 16;
  uint32_t local_y   = 16;
  size_t   global[2] = {RoundUpToMultiple(width, local_x), RoundUpToMultiple(height, local_y)};
  size_t   local[2]  = {local_x, local_y};

  cl_int err = clEnqueueNDRangeKernel(context.Queue(), kernel, 2, nullptr, global, local, 0,
                                      nullptr, nullptr);
  CheckOpenCl(err, "clEnqueueNDRangeKernel");
}

}  // namespace

void Bayer2x2ToRGB_RCD(opencl::OpenClImage& image, const BayerPattern2x2& pattern) {
  if (image.Empty()) {
    throw std::runtime_error("OpenCL Debayer RCD: input image is empty.");
  }
  if (image.Type() != CV_32FC1) {
    throw std::runtime_error("OpenCL Debayer RCD: expected CV_32FC1 Bayer input.");
  }

  const uint32_t in_width  = static_cast<uint32_t>(image.Width());
  const uint32_t in_height = static_cast<uint32_t>(image.Height());
  if (in_width == 0 || in_height == 0) {
    return;
  }

  const int out_width  = std::max(0, static_cast<int>(in_width) - 8);
  const int out_height = std::max(0, static_cast<int>(in_height) - 8);
  if (out_width <= 0 || out_height <= 0) {
    throw std::runtime_error("OpenCL Debayer RCD: image too small for RCD radius.");
  }

  auto& context = OpenClContext::Instance();
  if (!context.IsInitialized()) {
    context.Initialize();
  }

  // Allocate intermediate single-plane buffers at input resolution.
  opencl::OpenClImage r_img, g_img, b_img, vh_img, pq_img;
  r_img.Create(static_cast<int>(in_width), static_cast<int>(in_height), CV_32FC1);
  g_img.Create(static_cast<int>(in_width), static_cast<int>(in_height), CV_32FC1);
  b_img.Create(static_cast<int>(in_width), static_cast<int>(in_height), CV_32FC1);
  vh_img.Create(static_cast<int>(in_width), static_cast<int>(in_height), CV_32FC1);
  pq_img.Create(static_cast<int>(in_width), static_cast<int>(in_height), CV_32FC1);

  // Output RGBA buffer at the cropped resolution (RCD invalidates a 4-pixel border band).
  opencl::OpenClImage out_img;
  out_img.Create(out_width, out_height, CV_32FC4);

  const SinglePlaneParams plane_params{
      .width  = in_width,
      .height = in_height,
      .stride = in_width,
      .rgb_fc = {static_cast<uint32_t>(pattern.rgb_fc[0]),
                 static_cast<uint32_t>(pattern.rgb_fc[1]),
                 static_cast<uint32_t>(pattern.rgb_fc[2]),
                 static_cast<uint32_t>(pattern.rgb_fc[3])},
  };

  const MergeParams merge_params{
      .width        = in_width,
      .height       = in_height,
      .plane_stride = in_width,
      .rgba_stride  = static_cast<uint32_t>(out_width),
  };

  cl_program program = OpenClProgramLibrary::Instance().GetProgram("raw_processor_debayer_rcd");
  cl_int     err     = CL_SUCCESS;

  cl_mem raw_buf  = image.Buffer();
  cl_mem r_buf    = r_img.Buffer();
  cl_mem g_buf    = g_img.Buffer();
  cl_mem b_buf    = b_img.Buffer();
  cl_mem vh_buf   = vh_img.Buffer();
  cl_mem pq_buf   = pq_img.Buffer();
  cl_mem out_buf  = out_img.Buffer();
  const cl_uint zero = 0;

  auto set_mem = [&](cl_kernel kernel, cl_uint index, cl_mem buffer, const char* what) {
    err = clSetKernelArg(kernel, index, sizeof(cl_mem), &buffer);
    CheckOpenCl(err, what);
  };
  auto set_off = [&](cl_kernel kernel, cl_uint index, const char* what) {
    err = clSetKernelArg(kernel, index, sizeof(cl_uint), &zero);
    CheckOpenCl(err, what);
  };

  cl_kernel k0 = clCreateKernel(program, "rcd_init_and_vh", &err);
  CheckOpenCl(err, "clCreateKernel(init_and_vh)");
  set_mem(k0, 0, raw_buf, "clSetKernelArg(k0,0)");
  set_mem(k0, 1, r_buf, "clSetKernelArg(k0,1)");
  set_mem(k0, 2, g_buf, "clSetKernelArg(k0,2)");
  set_mem(k0, 3, b_buf, "clSetKernelArg(k0,3)");
  set_mem(k0, 4, vh_buf, "clSetKernelArg(k0,4)");
  err = clSetKernelArg(k0, 5, sizeof(plane_params), &plane_params);
  CheckOpenCl(err, "clSetKernelArg(k0,5)");
  set_off(k0, 6, "clSetKernelArg(k0,6)");
  set_off(k0, 7, "clSetKernelArg(k0,7)");
  set_off(k0, 8, "clSetKernelArg(k0,8)");
  set_off(k0, 9, "clSetKernelArg(k0,9)");
  set_off(k0, 10, "clSetKernelArg(k0,10)");
  DispatchKernel(k0, in_width, in_height);
  clReleaseKernel(k0);

  cl_kernel k1 = clCreateKernel(program, "rcd_green_at_rb", &err);
  CheckOpenCl(err, "clCreateKernel(green_at_rb)");
  set_mem(k1, 0, raw_buf, "clSetKernelArg(k1,0)");
  set_mem(k1, 1, vh_buf, "clSetKernelArg(k1,1)");
  set_mem(k1, 2, g_buf, "clSetKernelArg(k1,2)");
  err = clSetKernelArg(k1, 3, sizeof(plane_params), &plane_params);
  CheckOpenCl(err, "clSetKernelArg(k1,3)");
  set_off(k1, 4, "clSetKernelArg(k1,4)");
  set_off(k1, 5, "clSetKernelArg(k1,5)");
  set_off(k1, 6, "clSetKernelArg(k1,6)");
  DispatchKernel(k1, in_width, in_height);
  clReleaseKernel(k1);

  cl_kernel k2 = clCreateKernel(program, "rcd_pq_dir", &err);
  CheckOpenCl(err, "clCreateKernel(pq_dir)");
  set_mem(k2, 0, raw_buf, "clSetKernelArg(k2,0)");
  set_mem(k2, 1, pq_buf, "clSetKernelArg(k2,1)");
  err = clSetKernelArg(k2, 2, sizeof(plane_params), &plane_params);
  CheckOpenCl(err, "clSetKernelArg(k2,2)");
  set_off(k2, 3, "clSetKernelArg(k2,3)");
  set_off(k2, 4, "clSetKernelArg(k2,4)");
  DispatchKernel(k2, in_width, in_height);
  clReleaseKernel(k2);

  cl_kernel k3 = clCreateKernel(program, "rcd_rb_at_rb", &err);
  CheckOpenCl(err, "clCreateKernel(rb_at_rb)");
  set_mem(k3, 0, pq_buf, "clSetKernelArg(k3,0)");
  set_mem(k3, 1, g_buf, "clSetKernelArg(k3,1)");
  set_mem(k3, 2, r_buf, "clSetKernelArg(k3,2)");
  set_mem(k3, 3, b_buf, "clSetKernelArg(k3,3)");
  err = clSetKernelArg(k3, 4, sizeof(plane_params), &plane_params);
  CheckOpenCl(err, "clSetKernelArg(k3,4)");
  set_off(k3, 5, "clSetKernelArg(k3,5)");
  set_off(k3, 6, "clSetKernelArg(k3,6)");
  set_off(k3, 7, "clSetKernelArg(k3,7)");
  set_off(k3, 8, "clSetKernelArg(k3,8)");
  DispatchKernel(k3, in_width, in_height);
  clReleaseKernel(k3);

  cl_kernel k4 = clCreateKernel(program, "rcd_rb_at_g", &err);
  CheckOpenCl(err, "clCreateKernel(rb_at_g)");
  set_mem(k4, 0, vh_buf, "clSetKernelArg(k4,0)");
  set_mem(k4, 1, g_buf, "clSetKernelArg(k4,1)");
  set_mem(k4, 2, r_buf, "clSetKernelArg(k4,2)");
  set_mem(k4, 3, b_buf, "clSetKernelArg(k4,3)");
  err = clSetKernelArg(k4, 4, sizeof(plane_params), &plane_params);
  CheckOpenCl(err, "clSetKernelArg(k4,4)");
  set_off(k4, 5, "clSetKernelArg(k4,5)");
  set_off(k4, 6, "clSetKernelArg(k4,6)");
  set_off(k4, 7, "clSetKernelArg(k4,7)");
  set_off(k4, 8, "clSetKernelArg(k4,8)");
  DispatchKernel(k4, in_width, in_height);
  clReleaseKernel(k4);

  cl_kernel k5 = clCreateKernel(program, "rcd_merge_rgba", &err);
  CheckOpenCl(err, "clCreateKernel(merge_rgba)");
  set_mem(k5, 0, r_buf, "clSetKernelArg(k5,0)");
  set_mem(k5, 1, g_buf, "clSetKernelArg(k5,1)");
  set_mem(k5, 2, b_buf, "clSetKernelArg(k5,2)");
  set_mem(k5, 3, out_buf, "clSetKernelArg(k5,3)");
  err = clSetKernelArg(k5, 4, sizeof(plane_params), &plane_params);
  CheckOpenCl(err, "clSetKernelArg(k5,4)");
  err = clSetKernelArg(k5, 5, sizeof(merge_params), &merge_params);
  CheckOpenCl(err, "clSetKernelArg(k5,5)");
  set_off(k5, 6, "clSetKernelArg(k5,6)");
  set_off(k5, 7, "clSetKernelArg(k5,7)");
  set_off(k5, 8, "clSetKernelArg(k5,8)");
  set_off(k5, 9, "clSetKernelArg(k5,9)");
  DispatchKernel(k5, in_width, in_height);
  clReleaseKernel(k5);

  err = clFinish(context.Queue());
  CheckOpenCl(err, "clFinish");

  image = std::move(out_img);
}

}  // namespace OpenCL
}  // namespace alcedo

#endif
