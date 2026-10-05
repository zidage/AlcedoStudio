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
                               std::uint32_t height, cl_mem params, DisplayToAp1Output output_kind,
                               cl_event* event) {
  const char* kernel_name = output_kind == DisplayToAp1Output::LinearAp0
                                ? OpenCL::GpuDag::kDisplayToAp0LinearKernelName
                                : OpenCL::GpuDag::kDisplayToAp1KernelName;
  auto kernel = OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kDisplayToAp1ProgramName,
                                                        kernel_name);
  CheckOpenCl(clSetKernelArg(kernel, 0, sizeof(cl_mem), &src), "DisplayToAp1 arg0");
  CheckOpenCl(clSetKernelArg(kernel, 1, sizeof(cl_mem), &dst), "DisplayToAp1 arg1");
  CheckOpenCl(clSetKernelArg(kernel, 2, sizeof(cl_mem), &params), "DisplayToAp1 arg2");
  const std::size_t local[2]  = {16, 16};
  const std::size_t global[2] = {((static_cast<std::size_t>(width) + 15) / 16) * 16,
                                 ((static_cast<std::size_t>(height) + 15) / 16) * 16};
  CheckOpenCl(clEnqueueNDRangeKernel(queue, kernel, 2, nullptr, global, local, 0, nullptr, event),
              "DisplayToAp1 enqueue");
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
