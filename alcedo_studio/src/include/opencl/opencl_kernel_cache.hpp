//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace alcedo {

/**
 * @brief Process-wide registry handing out per-thread OpenCL kernel objects.
 *
 * clSetKernelArg mutates kernel state and is the one OpenCL call that is not
 * thread-safe, so parallel render threads must never share one cl_kernel
 * object. GetKernel therefore caches one kernel per calling thread per
 * (program, kernel) pair and releases it at thread exit. Programs stay shared
 * through OpenClProgramLibrary; registration is not performed here.
 */
class OpenClKernelCache {
 public:
  OpenClKernelCache(const OpenClKernelCache&)                    = delete;
  auto operator=(const OpenClKernelCache&) -> OpenClKernelCache& = delete;

  static auto Instance() -> OpenClKernelCache&;

  /**
   * @brief Return the calling thread's cached kernel, creating it on miss.
   * @param program_name Registered OpenClProgramLibrary program name.
   * @param kernel_name Kernel name inside that program.
   * @return Borrowed cl_kernel owned by the calling thread's store until
   *         thread exit. Only the owning thread may bind arguments on it.
   * @throws std::runtime_error if the program is missing, build fails, or the kernel
   *         name is absent. The message includes program name, kernel name, and
   *         the OpenCL status or build log.
   */
  auto GetKernel(std::string_view program_name, std::string_view kernel_name) -> cl_kernel;

  /**
   * @brief Whether the calling thread already owns this kernel object.
   */
  [[nodiscard]] auto IsCached(std::string_view program_name, std::string_view kernel_name) const
      -> bool;

  [[nodiscard]] auto CreateCount() const -> std::uint64_t;
  [[nodiscard]] auto HitCount() const -> std::uint64_t;

 private:
  OpenClKernelCache() = default;

  struct Key {
    std::string program_name;
    std::string kernel_name;

    [[nodiscard]] auto operator==(const Key& other) const -> bool {
      return program_name == other.program_name && kernel_name == other.kernel_name;
    }
  };

  struct KeyHash {
    auto operator()(const Key& key) const -> std::size_t {
      return std::hash<std::string>{}(key.program_name) ^
             (std::hash<std::string>{}(key.kernel_name) << 1);
    }
  };

  /** @brief Per-thread kernel store; destructor releases all owned kernels. */
  struct KernelStore {
    std::unordered_map<Key, cl_kernel, KeyHash> kernels;
    ~KernelStore();
  };

  /** @brief The calling thread's kernel store (constructed lazily). */
  static auto ThreadKernels() -> KernelStore&;

  std::atomic<std::uint64_t> create_count_{0};
  std::atomic<std::uint64_t> hit_count_{0};
};

}  // namespace alcedo

#endif  // HAVE_OPENCL
