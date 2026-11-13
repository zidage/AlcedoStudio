//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_display_to_ap1_pass.hpp"

#include <algorithm>
#include <stdexcept>

#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "opencl/opencl_check.hpp"
#include "opencl/opencl_kernel_cache.hpp"

namespace alcedo {

OpenClDisplayToAp1Parameters::~OpenClDisplayToAp1Parameters() {
  if (buffer_ != nullptr) {
    clReleaseMemObject(buffer_);
  }
}

auto OpenClDisplayToAp1Parameters::Upload(cl_context context, cl_command_queue queue,
                                          std::span<const float> packed) -> cl_mem {
  if (packed.empty()) {
    throw std::runtime_error("OpenCL DisplayToAp1: empty parameter block");
  }
  if (buffer_ != nullptr && uploaded_.size() == packed.size() &&
      std::equal(packed.begin(), packed.end(), uploaded_.begin())) {
    return buffer_;
  }
  if (packed.size() > capacity_) {
    if (buffer_ != nullptr) {
      clReleaseMemObject(buffer_);
      buffer_ = nullptr;
    }
    cl_int status = CL_SUCCESS;
    buffer_ = clCreateBuffer(context, CL_MEM_READ_ONLY, packed.size_bytes(), nullptr, &status);
    CheckOpenCl(status, "OpenCL DisplayToAp1 parameter buffer");
    capacity_ = packed.size();
  }
  CheckOpenCl(clEnqueueWriteBuffer(queue, buffer_, CL_TRUE, 0, packed.size_bytes(), packed.data(),
                                   0, nullptr, nullptr),
              "OpenCL DisplayToAp1 parameter upload");
  uploaded_.assign(packed.begin(), packed.end());
  ++upload_count_;
  return buffer_;
}

void EnqueueOpenClDisplayToAp1(cl_command_queue queue, cl_mem src, cl_mem dst, std::uint32_t width,
                               std::uint32_t height, cl_mem params,
                               std::uint32_t params_offset_floats, DisplayToAp1Output output_kind,
                               cl_event* event) {
  const char* kernel_name = output_kind == DisplayToAp1Output::LinearAp0
                                ? OpenCL::GpuDag::kDisplayToAp0LinearKernelName
                                : OpenCL::GpuDag::kDisplayToAp1KernelName;
  auto kernel = OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kDisplayToAp1ProgramName,
                                                        kernel_name);
  CheckOpenCl(clSetKernelArg(kernel, 0, sizeof(cl_mem), &src), "DisplayToAp1 arg0");
  CheckOpenCl(clSetKernelArg(kernel, 1, sizeof(cl_mem), &dst), "DisplayToAp1 arg1");
  CheckOpenCl(clSetKernelArg(kernel, 2, sizeof(cl_mem), &params), "DisplayToAp1 arg2");
  CheckOpenCl(clSetKernelArg(kernel, 3, sizeof(cl_uint), &params_offset_floats),
              "DisplayToAp1 arg3");
  const std::size_t local[2]  = {16, 16};
  const std::size_t global[2] = {((static_cast<std::size_t>(width) + 15) / 16) * 16,
                                 ((static_cast<std::size_t>(height) + 15) / 16) * 16};
  CheckOpenCl(clEnqueueNDRangeKernel(queue, kernel, 2, nullptr, global, local, 0, nullptr, event),
              "DisplayToAp1 enqueue");
}

void EnqueueOpenClLinearizeRaster(cl_command_queue queue, cl_mem src,
                                  std::uint32_t src_offset_bytes, HostPixelFormat format,
                                  std::uint32_t width, std::uint32_t height, cl_mem params,
                                  std::uint32_t params_offset_floats, cl_mem dst,
                                  std::uint32_t dst_offset_floats, cl_event* event) {
  if (format == HostPixelFormat::U16Cfa) {
    throw std::runtime_error("EnqueueOpenClLinearizeRaster: a CFA plane is not raster input");
  }
  auto kernel = OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kDisplayToAp1ProgramName,
                                                        OpenCL::GpuDag::kLinearizeRasterKernelName);
  const cl_uint format_value = static_cast<cl_uint>(format);
  CheckOpenCl(clSetKernelArg(kernel, 0, sizeof(cl_mem), &src), "LinearizeRaster arg0");
  CheckOpenCl(clSetKernelArg(kernel, 1, sizeof(cl_uint), &src_offset_bytes),
              "LinearizeRaster arg1");
  CheckOpenCl(clSetKernelArg(kernel, 2, sizeof(cl_uint), &format_value), "LinearizeRaster arg2");
  CheckOpenCl(clSetKernelArg(kernel, 3, sizeof(cl_uint), &width), "LinearizeRaster arg3");
  CheckOpenCl(clSetKernelArg(kernel, 4, sizeof(cl_uint), &height), "LinearizeRaster arg4");
  CheckOpenCl(clSetKernelArg(kernel, 5, sizeof(cl_mem), &params), "LinearizeRaster arg5");
  CheckOpenCl(clSetKernelArg(kernel, 6, sizeof(cl_uint), &params_offset_floats),
              "LinearizeRaster arg6");
  CheckOpenCl(clSetKernelArg(kernel, 7, sizeof(cl_mem), &dst), "LinearizeRaster arg7");
  CheckOpenCl(clSetKernelArg(kernel, 8, sizeof(cl_uint), &dst_offset_floats),
              "LinearizeRaster arg8");
  const std::size_t local[2]  = {16, 16};
  const std::size_t global[2] = {((static_cast<std::size_t>(width) + 15) / 16) * 16,
                                 ((static_cast<std::size_t>(height) + 15) / 16) * 16};
  CheckOpenCl(clEnqueueNDRangeKernel(queue, kernel, 2, nullptr, global, local, 0, nullptr, event),
              "LinearizeRaster enqueue");
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
