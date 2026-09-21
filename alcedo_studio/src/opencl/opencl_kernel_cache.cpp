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

OpenClKernelCache::KernelStore::~KernelStore() {
  for (auto& [_, kernel] : kernels) {
    if (kernel != nullptr) {
      clReleaseKernel(kernel);
      NoteOpenClReleaseKernel();
    }
  }
}

auto OpenClKernelCache::ThreadKernels() -> KernelStore& {
  thread_local KernelStore store;
  return store;
}

auto OpenClKernelCache::Instance() -> OpenClKernelCache& {
  static OpenClKernelCache cache;
  return cache;
}

auto OpenClKernelCache::GetKernel(std::string_view program_name, std::string_view kernel_name)
    -> cl_kernel {
  auto& kernels = ThreadKernels().kernels;
  Key   key{std::string(program_name), std::string(kernel_name)};
  if (const auto it = kernels.find(key); it != kernels.end()) {
    hit_count_.fetch_add(1, std::memory_order_relaxed);
    return it->second;
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
  kernels.emplace(std::move(key), kernel);
  create_count_.fetch_add(1, std::memory_order_relaxed);
  return kernel;
}

auto OpenClKernelCache::IsCached(std::string_view program_name, std::string_view kernel_name) const
    -> bool {
  return ThreadKernels().kernels.contains(
      Key{std::string(program_name), std::string(kernel_name)});
}

auto OpenClKernelCache::CreateCount() const -> std::uint64_t {
  return create_count_.load(std::memory_order_relaxed);
}

auto OpenClKernelCache::HitCount() const -> std::uint64_t {
  return hit_count_.load(std::memory_order_relaxed);
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
