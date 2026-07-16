//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <array>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>

namespace alcedo {

struct OpenClDeviceCapabilities {
  std::string           name;
  std::string           vendor;
  std::string           driver_version;
  std::string           device_version;
  std::string           opencl_c_version;
  std::string           extensions;
  cl_device_type        device_type                 = 0;
  cl_uint               compute_units               = 0;
  cl_uint               max_clock_frequency_mhz     = 0;
  cl_ulong              global_memory_bytes         = 0;
  cl_ulong              max_single_allocation_bytes = 0;
  cl_ulong              local_memory_bytes          = 0;
  size_t                max_work_group_size         = 0;
  cl_uint               max_work_item_dimensions    = 0;
  std::array<size_t, 3> max_work_item_sizes         = {};
  bool                  image_support               = false;
  bool                  available                   = false;
  bool                  compiler_available          = false;

  [[nodiscard]] auto SupportsExtension(std::string_view extension) const -> bool;
};

struct OpenClInitializationOptions {
  // Case-insensitive substring matched against "<vendor> <device name>".
  // Useful for user preferences such as "nvidia" or "intel arc". If omitted,
  // Initialize() also checks ALCEDO_OPENCL_DEVICE as a lightweight user override.
  std::optional<std::string> preferred_device;

  // Optional ID3D11Device* on Windows. When set, OpenCL initialization selects a
  // device that supports cl_khr_d3d11_sharing with that D3D11 device and creates
  // the context with the matching D3D11 sharing properties.
  void*                      d3d11_device = nullptr;

  // Optional native OpenGL context/display handles. On Windows these are HGLRC
  // and HDC. When set, OpenCL initialization selects a device that can share
  // with the GL context and creates a cl_khr_gl_sharing context.
  void*                      gl_context = nullptr;
  void*                      gl_device_context = nullptr;
};

class OpenClContext {
 private:
  cl_platform_id           platform_ = nullptr;
  cl_device_id             device_   = nullptr;
  cl_context               context_  = nullptr;
  cl_command_queue         queue_    = nullptr;
  // Development-only second in-order queue with CL_QUEUE_PROFILING_ENABLE.
  cl_command_queue         profiling_queue_ = nullptr;
  // When set, Queue() returns this instead of the product queue.
  mutable cl_command_queue queue_override_ = nullptr;
  OpenClDeviceCapabilities capabilities_;
  bool                     initialized_              = false;
  bool                     initialization_attempted_ = false;
  bool                     d3d11_sharing_enabled_    = false;
  bool                     gl_sharing_enabled_       = false;
  std::string              last_initialization_error_;
  mutable std::mutex       mutex_;

  OpenClContext() = default;

  ~OpenClContext();

 public:
  OpenClContext(const OpenClContext&)                      = delete;
  auto operator=(const OpenClContext&) -> OpenClContext&   = delete;
  OpenClContext(OpenClContext&&)                           = delete;
  auto        operator=(OpenClContext&&) -> OpenClContext& = delete;

  static auto Instance() -> OpenClContext&;

  // Selects the best usable GPU by default. If options.preferred_device is set,
  // it takes precedence when a matching usable device exists.
  void        Initialize(const OpenClInitializationOptions& options = {});

  // Non-throwing path for optional fallback logic. If OpenCL cannot be
  // initialized, callers can continue with the CPU path instead.
  auto        TryInitialize(const OpenClInitializationOptions& options = {}) -> bool;

  auto        IsInitialized() const -> bool;
  auto        InitializationAttempted() const -> bool;
  auto        LastInitializationError() const -> std::string;

  auto        Platform() const -> cl_platform_id;
  auto        Device() const -> cl_device_id;
  auto        Context() const -> cl_context;
  // Product command queue, or the installed development override when active.
  auto        Queue() const -> cl_command_queue;
  // Product queue only (never the profiling override).
  auto        ProductQueue() const -> cl_command_queue;
  auto        D3D11SharingEnabled() const -> bool;
  auto        GLSharingEnabled() const -> bool;
  auto        Capabilities() const -> const OpenClDeviceCapabilities&;

  // Development-only: create (once) and install a profiling-enabled in-order queue
  // as Queue() for event-timestamp telemetry. Does not replace the product queue
  // object; clear with ClearQueueOverride(). Not for product decode paths.
  void        InstallProfilingQueueOverride();
  void        ClearQueueOverride();
  [[nodiscard]] auto ProfilingQueueInstalled() const -> bool;
  [[nodiscard]] auto HasProfilingQueue() const -> bool;
};

}  // namespace alcedo

#endif
