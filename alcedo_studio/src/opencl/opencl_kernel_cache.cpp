//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "opencl/opencl_kernel_cache.hpp"

#include <stdexcept>
#include <utility>

#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_program_library.hpp"

namespace alcedo {

OpenClKernelCache::~OpenClKernelCache() {
  for (auto& [_, kernel] : kernels_) {
    if (kernel != nullptr) {
      clReleaseKernel(kernel);
      NoteOpenClReleaseKernel();
    }
  }
}

auto OpenClKernelCache::Instance() -> OpenClKernelCache& {
  static OpenClKernelCache cache;
  return cache;
}

auto OpenClKernelCache::GetKernel(std::string_view program_name, std::string_view kernel_name)
    -> cl_kernel {
  Key key{std::string(program_name), std::string(kernel_name)};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto                  it = kernels_.find(key);
    if (it != kernels_.end()) {
      ++hit_count_;
      return it->second;
    }
  }

  cl_program program = OpenClProgramLibrary::Instance().GetProgram(key.program_name);
  cl_int     error   = CL_SUCCESS;
  cl_kernel  kernel  = clCreateKernel(program, key.kernel_name.c_str(), &error);
  if (error != CL_SUCCESS || kernel == nullptr) {
    throw std::runtime_error("OpenClKernelCache: failed to create kernel '" + key.kernel_name +
                             "' from program '" + key.program_name + "': OpenCL error " +
                             std::to_string(error));
  }
  NoteOpenClCreateKernel();

  std::lock_guard<std::mutex> lock(mutex_);
  const auto                  it = kernels_.find(key);
  if (it != kernels_.end()) {
    clReleaseKernel(kernel);
    NoteOpenClReleaseKernel();
    ++hit_count_;
    return it->second;
  }
  kernels_.emplace(std::move(key), kernel);
  ++create_count_;
  return kernel;
}

auto OpenClKernelCache::IsCached(std::string_view program_name, std::string_view kernel_name) const
    -> bool {
  std::lock_guard<std::mutex> lock(mutex_);
  return kernels_.contains(Key{std::string(program_name), std::string(kernel_name)});
}

auto OpenClKernelCache::CreateCount() const -> std::uint64_t {
  std::lock_guard<std::mutex> lock(mutex_);
  return create_count_;
}

auto OpenClKernelCache::HitCount() const -> std::uint64_t {
  std::lock_guard<std::mutex> lock(mutex_);
  return hit_count_;
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
