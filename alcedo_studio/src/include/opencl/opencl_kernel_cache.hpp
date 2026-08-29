//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace alcedo {

/**
 * @brief Process-wide cache of OpenCL kernels keyed by program name plus kernel name.
 *
 * Owns kernel lifetime. Does not register programs; callers must use
 * OpenClProgramLibrary. Not safe to destroy while kernels are bound to in-flight
 * commands. GetKernel may compile the named program on first use.
 */
class OpenClKernelCache {
 public:
  OpenClKernelCache(const OpenClKernelCache&)                    = delete;
  auto operator=(const OpenClKernelCache&) -> OpenClKernelCache& = delete;

  static auto Instance() -> OpenClKernelCache&;

  /**
   * @brief Return a cached kernel, creating it on miss.
   * @param program_name Registered OpenClProgramLibrary program name.
   * @param kernel_name Kernel name inside that program.
   * @return Borrowed cl_kernel owned by this cache until process teardown.
   * @throws std::runtime_error if the program is missing, build fails, or the kernel
   *         name is absent. The message includes program name, kernel name, and
   *         the OpenCL status or build log.
   */
  auto GetKernel(std::string_view program_name, std::string_view kernel_name) -> cl_kernel;

  [[nodiscard]] auto IsCached(std::string_view program_name, std::string_view kernel_name) const
      -> bool;

  [[nodiscard]] auto CreateCount() const -> std::uint64_t;
  [[nodiscard]] auto HitCount() const -> std::uint64_t;

 private:
  OpenClKernelCache() = default;
  ~OpenClKernelCache();

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

  mutable std::mutex                        mutex_;
  std::unordered_map<Key, cl_kernel, KeyHash> kernels_;
  std::uint64_t                             create_count_ = 0;
  std::uint64_t                             hit_count_    = 0;
};

}  // namespace alcedo

#endif  // HAVE_OPENCL
