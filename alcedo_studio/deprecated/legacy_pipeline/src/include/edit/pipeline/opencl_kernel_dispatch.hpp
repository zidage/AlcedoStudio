//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <CL/cl.h>

#include <string>

#include "opencl/opencl_context.hpp"
#include "opencl/opencl_program_library.hpp"

namespace alcedo::opencl_detail {

class OpenClKernelHandle {
 public:
  constexpr OpenClKernelHandle(const char* program_name, const char* kernel_name)
      : program_name_(program_name), kernel_name_(kernel_name) {}

  OpenClKernelHandle(const OpenClKernelHandle&)                    = delete;
  auto operator=(const OpenClKernelHandle&) -> OpenClKernelHandle& = delete;

  ~OpenClKernelHandle() { Release(); }

  void Ensure(const char* owner_label) {
    if (kernel_ != nullptr) {
      return;
    }

    cl_program program = OpenClProgramLibrary::Instance().GetProgram(program_name_);
    if (program == nullptr) {
      throw std::runtime_error(std::string(owner_label) + ": failed to get OpenCL program '" +
                               program_name_ + "'.");
    }

    cl_int err = CL_SUCCESS;
    kernel_    = clCreateKernel(program, kernel_name_, &err);
    if (err != CL_SUCCESS || kernel_ == nullptr) {
      throw std::runtime_error(std::string(owner_label) + ": failed to create OpenCL kernel '" +
                               kernel_name_ + "' with error " + std::to_string(err) + ".");
    }
  }

  void Release() {
    if (kernel_ != nullptr) {
      clReleaseKernel(kernel_);
      kernel_ = nullptr;
    }
  }

  [[nodiscard]] auto Get() const -> cl_kernel { return kernel_; }

 private:
  const char* program_name_ = nullptr;
  const char* kernel_name_  = nullptr;
  cl_kernel   kernel_       = nullptr;
};

template <typename Derived>
class OpenClKernelStage {
 protected:
  static void EnqueueKernel2D(cl_kernel kernel, int width, int height, const char* label) {
    auto&        context        = OpenClContext::Instance();
    size_t       global_size[2] = {static_cast<size_t>(width), static_cast<size_t>(height)};
    const cl_int err = clEnqueueNDRangeKernel(context.Queue(), kernel, 2, nullptr, global_size,
                                              nullptr, 0, nullptr, nullptr);
    if (err != CL_SUCCESS) {
      throw std::runtime_error(std::string(Derived::kStageLabel) + ": failed to enqueue " + label +
                               " with error " + std::to_string(err) + ".");
    }
  }

  template <typename... Args>
  static void SetKernelArgs(cl_kernel kernel, const char* label, const Args&... args) {
    cl_int  err       = CL_SUCCESS;
    cl_uint arg_index = 0;
    (SetOneKernelArg(kernel, arg_index, err, args), ...);
    if (err != CL_SUCCESS) {
      throw std::runtime_error(std::string(Derived::kStageLabel) + ": failed to set " + label +
                               " arguments.");
    }
  }

 private:
  template <typename T>
  static void SetOneKernelArg(cl_kernel kernel, cl_uint& arg_index, cl_int& err, const T& value) {
    err |= clSetKernelArg(kernel, arg_index++, sizeof(T), &value);
  }
};

}  // namespace alcedo::opencl_detail

#endif  // HAVE_OPENCL
