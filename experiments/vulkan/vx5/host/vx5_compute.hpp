//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX5 experiment: memory blocks on the pipeline VkDevice, the touch pass that shows the speed of
// each block, the fault kernel, and the budget values of VK_EXT_memory_budget and DXGI.
// Experiment code: it does not merge.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

#include "vx2_vulkan.hpp"

namespace vx5 {

using vx2::VulkanContext;

struct HeapBudget {
  VkDeviceSize      size   = 0;
  VkDeviceSize      budget = 0;
  VkDeviceSize      usage  = 0;
  VkMemoryHeapFlags flags  = 0;
};

/// VK_EXT_memory_budget values of each heap. The query takes the physical device: the values are
/// those of the process on the adapter.
auto QueryBudget(VkPhysicalDevice physical) -> std::vector<HeapBudget>;

struct DxgiBudget {
  bool         valid          = false;
  std::uint64_t local_budget   = 0;
  std::uint64_t local_usage    = 0;
  std::uint64_t nonlocal_budget = 0;
  std::uint64_t nonlocal_usage  = 0;
};

/// IDXGIAdapter3::QueryVideoMemoryInfo of the adapter with @p luid (this process).
auto QueryDxgi(const std::uint8_t luid[8]) -> DxgiBudget;

/// Free physical memory of the computer (GlobalMemoryStatusEx).
auto FreePhysicalMiB() -> std::uint64_t;

struct Block {
  VkBuffer        buffer   = VK_NULL_HANDLE;
  VkDeviceMemory  memory   = VK_NULL_HANDLE;
  VkDeviceAddress address  = 0;
  VkDeviceSize    size     = 0;
  float           priority = -1.0f;  // < 0: no VkMemoryPriorityAllocateInfoEXT
  int             index    = 0;
  VkDevice        device   = VK_NULL_HANDLE;
};

/// The compute objects of VX5 on one device (normally the pipeline device).
class Compute {
 public:
  Compute(VulkanContext& ctx, VkDevice device, const std::string& spv_path);
  ~Compute();
  Compute(const Compute&)            = delete;
  Compute& operator=(const Compute&) = delete;

  /// One dedicated device-local allocation of @p size bytes with a device address. The result is
  /// the VkResult of vkAllocateMemory; @p ms is the time of the call.
  auto Allocate(VkDeviceSize size, float priority, Block& block, double& ms) -> VkResult;
  void Free(Block& block);
  /// vkCmdFillBuffer of the whole block (first use: Windows makes the memory resident).
  auto Fill(const Block& block) -> VkResult;
  /// One submission: a touch dispatch for each block with a timestamp after each. @p ms gets the
  /// GPU time of each dispatch.
  auto Touch(const std::vector<const Block*>& blocks, std::vector<double>& ms) -> VkResult;
  /// One dispatch of the fault kernel: reads @p bad and writes @p out. Waits for at most
  /// @p wait_ms; @p wait gets the time to the result of the fence wait.
  auto Fault(VkDeviceAddress bad, VkDeviceAddress out, double wait_ms, double& wait) -> VkResult;
  /// An empty submission and its fence wait (a device check).
  auto Ping(double wait_ms) -> VkResult;
  /// vkGetDeviceFaultInfoEXT, when the device has VK_EXT_device_fault.
  auto FaultReport() -> std::string;

  [[nodiscard]] auto Device() const -> VkDevice { return dev_; }

 private:
  auto SubmitAndWait(double wait_ms, double* wait) -> VkResult;
  auto Begin() -> VkResult;

  VulkanContext&   ctx_;
  VkDevice         dev_;
  VkQueue          queue_    = VK_NULL_HANDLE;
  VkShaderModule   module_   = VK_NULL_HANDLE;
  VkPipelineLayout layout_   = VK_NULL_HANDLE;
  VkPipeline       touch_    = VK_NULL_HANDLE;
  VkPipeline       fault_    = VK_NULL_HANDLE;
  VkCommandPool    pool_     = VK_NULL_HANDLE;
  VkCommandBuffer  cb_       = VK_NULL_HANDLE;
  VkFence          fence_    = VK_NULL_HANDLE;
  VkQueryPool      queries_  = VK_NULL_HANDLE;
  std::uint32_t    query_count_ = 0;
  double           timestamp_ns_ = 1.0;
  std::uint32_t    device_local_type_ = 0;
};

/// Speed of a touch pass of @p bytes in @p ms: read and write, GB/s (10^9 bytes).
inline auto Gbps(VkDeviceSize bytes, double ms) -> double {
  return ms > 0 ? 2.0 * static_cast<double>(bytes) / (ms * 1e6) : 0.0;
}

}  // namespace vx5
