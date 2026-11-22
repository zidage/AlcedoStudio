//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX5 experiment: memory blocks, touch pass, fault kernel, budget values. Experiment code: it does
// not merge.
#include "vx5_compute.hpp"

#include <windows.h>

#include <dxgi1_4.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "vx2_common.hpp"

namespace vx5 {

using vx2::Log;
using vx2::NowNs;
using vx2::VkCheck;

namespace {

constexpr std::uint32_t kTouchGroups = 1024;  // grid-stride loop of vx5_touch
constexpr std::uint32_t kMaxQueries  = 512;

auto ReadSpirv(const std::string& path) -> std::vector<std::uint32_t> {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) throw std::runtime_error("cannot open " + path);
  const auto size = static_cast<std::size_t>(file.tellg());
  std::vector<std::uint32_t> words(size / 4);
  file.seekg(0);
  file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(size));
  return words;
}

struct Push {
  std::uint64_t a = 0;
  std::uint64_t b = 0;
};

}  // namespace

auto QueryBudget(VkPhysicalDevice physical) -> std::vector<HeapBudget> {
  VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
  VkPhysicalDeviceMemoryProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                                          &budget};
  vkGetPhysicalDeviceMemoryProperties2(physical, &props);
  std::vector<HeapBudget> heaps;
  for (std::uint32_t i = 0; i < props.memoryProperties.memoryHeapCount; ++i) {
    heaps.push_back({props.memoryProperties.memoryHeaps[i].size, budget.heapBudget[i],
                     budget.heapUsage[i], props.memoryProperties.memoryHeaps[i].flags});
  }
  return heaps;
}

auto QueryDxgi(const std::uint8_t luid[8]) -> DxgiBudget {
  DxgiBudget      result;
  IDXGIFactory4*  factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory)))) {
    return result;
  }
  LUID id{};
  std::memcpy(&id, luid, sizeof(id));
  IDXGIAdapter3* adapter = nullptr;
  if (SUCCEEDED(factory->EnumAdapterByLuid(id, __uuidof(IDXGIAdapter3),
                                           reinterpret_cast<void**>(&adapter)))) {
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
    if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
        SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal))) {
      result.valid           = true;
      result.local_budget    = local.Budget;
      result.local_usage     = local.CurrentUsage;
      result.nonlocal_budget = nonlocal.Budget;
      result.nonlocal_usage  = nonlocal.CurrentUsage;
    }
    adapter->Release();
  }
  factory->Release();
  return result;
}

auto FreePhysicalMiB() -> std::uint64_t {
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  GlobalMemoryStatusEx(&status);
  return status.ullAvailPhys >> 20;
}

Compute::Compute(VulkanContext& ctx, VkDevice device, const std::string& spv_path)
    : ctx_(ctx), dev_(device) {
  queue_ = device == ctx_.pipe_device ? ctx_.pipeline_queue : ctx_.qt_queue;
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(ctx_.physical, &props);
  timestamp_ns_ = props.limits.timestampPeriod;

  // A device-local type without host access when the device has one (NVIDIA: not the BAR type).
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(ctx_.physical, &mp);
  bool found = false;
  for (int pass = 0; pass < 2 && !found; ++pass) {
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
      const auto f = mp.memoryTypes[i].propertyFlags;
      if (!(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
      if (pass == 0 && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
      device_local_type_ = i;
      found              = true;
      break;
    }
  }
  if (!found) throw std::runtime_error("no device-local memory type");

  const auto words = ReadSpirv(spv_path);
  VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  sm.codeSize = words.size() * 4;
  sm.pCode    = words.data();
  VkCheck(vkCreateShaderModule(dev_, &sm, nullptr, &module_), "vkCreateShaderModule");
  const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.pushConstantRangeCount = 1;
  pl.pPushConstantRanges    = &range;
  VkCheck(vkCreatePipelineLayout(dev_, &pl, nullptr, &layout_), "vkCreatePipelineLayout");
  for (auto [entry, pipeline] : {std::pair{"vx5_touch", &touch_}, std::pair{"vx5_fault", &fault_}}) {
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cp.stage  = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_COMPUTE_BIT, module_, entry, nullptr};
    cp.layout = layout_;
    VkCheck(vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &cp, nullptr, pipeline),
            "vkCreateComputePipelines");
  }
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool.queueFamilyIndex = ctx_.family;
  VkCheck(vkCreateCommandPool(dev_, &pool, nullptr, &pool_), "vkCreateCommandPool");
  VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  a.commandPool        = pool_;
  a.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  a.commandBufferCount = 1;
  VkCheck(vkAllocateCommandBuffers(dev_, &a, &cb_), "vkAllocateCommandBuffers");
  VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkCheck(vkCreateFence(dev_, &f, nullptr, &fence_), "vkCreateFence");
  VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qp.queryType  = VK_QUERY_TYPE_TIMESTAMP;
  qp.queryCount = kMaxQueries;
  VkCheck(vkCreateQueryPool(dev_, &qp, nullptr, &queries_), "vkCreateQueryPool");
  query_count_ = kMaxQueries;
}

Compute::~Compute() {
  // On a lost device the wait returns VK_ERROR_DEVICE_LOST at once; destruction is permitted.
  vkWaitForFences(dev_, 1, &fence_, VK_TRUE, 1'000'000'000ull);
  vkDestroyQueryPool(dev_, queries_, nullptr);
  vkDestroyFence(dev_, fence_, nullptr);
  vkDestroyCommandPool(dev_, pool_, nullptr);
  vkDestroyPipeline(dev_, touch_, nullptr);
  vkDestroyPipeline(dev_, fault_, nullptr);
  vkDestroyPipelineLayout(dev_, layout_, nullptr);
  vkDestroyShaderModule(dev_, module_, nullptr);
}

auto Compute::Allocate(VkDeviceSize size, float priority, Block& block, double& ms) -> VkResult {
  block          = {};
  block.device   = dev_;
  block.priority = priority;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size  = size;
  info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkCheck(vkCreateBuffer(dev_, &info, nullptr, &block.buffer), "vkCreateBuffer");
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(dev_, block.buffer, &req);
  if (!(req.memoryTypeBits & (1u << device_local_type_))) {
    throw std::runtime_error("the buffer cannot use the device-local type");
  }
  VkMemoryPriorityAllocateInfoEXT prio{VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT};
  prio.priority = priority;
  VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated.buffer = block.buffer;
  if (priority >= 0 && ctx_.memory_priority_enabled) dedicated.pNext = &prio;
  VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &dedicated};
  flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
  a.allocationSize  = req.size;
  a.memoryTypeIndex = device_local_type_;
  const std::int64_t t0 = NowNs();
  const VkResult     r  = vkAllocateMemory(dev_, &a, nullptr, &block.memory);
  ms                    = (NowNs() - t0) * 1e-6;
  if (r != VK_SUCCESS) {
    vkDestroyBuffer(dev_, block.buffer, nullptr);
    block = {};
    return r;
  }
  VkCheck(vkBindBufferMemory(dev_, block.buffer, block.memory, 0), "vkBindBufferMemory");
  VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  ai.buffer     = block.buffer;
  block.address = vkGetBufferDeviceAddress(dev_, &ai);
  block.size    = size;
  return VK_SUCCESS;
}

void Compute::Free(Block& block) {
  if (block.buffer != VK_NULL_HANDLE) vkDestroyBuffer(dev_, block.buffer, nullptr);
  if (block.memory != VK_NULL_HANDLE) vkFreeMemory(dev_, block.memory, nullptr);
  block = {};
}

auto Compute::Begin() -> VkResult {
  VkResult r = vkResetCommandBuffer(cb_, 0);
  if (r != VK_SUCCESS) return r;
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  return vkBeginCommandBuffer(cb_, &begin);
}

auto Compute::SubmitAndWait(double wait_ms, double* wait) -> VkResult {
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers    = &cb_;
  VkResult r;
  {
    std::lock_guard<std::mutex> lock(ctx_.pipeline_submit_mutex);
    r = vkQueueSubmit(queue_, 1, &si, fence_);
  }
  if (r != VK_SUCCESS) return r;
  const std::int64_t t0 = NowNs();
  r = vkWaitForFences(dev_, 1, &fence_, VK_TRUE, static_cast<std::uint64_t>(wait_ms * 1e6));
  if (wait) *wait = (NowNs() - t0) * 1e-6;
  if (r == VK_SUCCESS) vkResetFences(dev_, 1, &fence_);
  return r;
}

auto Compute::Fill(const Block& block) -> VkResult {
  VkResult r = Begin();
  if (r != VK_SUCCESS) return r;
  vkCmdFillBuffer(cb_, block.buffer, 0, VK_WHOLE_SIZE, 0x3f800000u);
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                       1, &mb, 0, nullptr, 0, nullptr);
  r = vkEndCommandBuffer(cb_);
  if (r != VK_SUCCESS) return r;
  return SubmitAndWait(60'000.0, nullptr);
}

auto Compute::Touch(const std::vector<const Block*>& blocks, std::vector<double>& ms) -> VkResult {
  ms.assign(blocks.size(), 0.0);
  if (blocks.size() + 1 > query_count_) throw std::runtime_error("too many blocks for one pass");
  VkResult r = Begin();
  if (r != VK_SUCCESS) return r;
  const auto n = static_cast<std::uint32_t>(blocks.size()) + 1;
  vkCmdResetQueryPool(cb_, queries_, 0, n);
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       0, 1, &mb, 0, nullptr, 0, nullptr);
  vkCmdWriteTimestamp(cb_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, 0);
  vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_COMPUTE, touch_);
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    const Push p{blocks[i]->address, blocks[i]->size / 16};
    vkCmdPushConstants(cb_, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cb_, kTouchGroups, 1, 1);
    // Each dispatch ends before the next begins, so the timestamp after it is its own time.
    vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    vkCmdWriteTimestamp(cb_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_,
                        static_cast<std::uint32_t>(i) + 1);
  }
  r = vkEndCommandBuffer(cb_);
  if (r != VK_SUCCESS) return r;
  r = SubmitAndWait(120'000.0, nullptr);
  if (r != VK_SUCCESS) return r;
  std::vector<std::uint64_t> t(n);
  r = vkGetQueryPoolResults(dev_, queries_, 0, n, n * 8, t.data(), 8,
                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
  if (r != VK_SUCCESS) return r;
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    ms[i] = static_cast<double>(t[i + 1] - t[i]) * timestamp_ns_ * 1e-6;
  }
  return VK_SUCCESS;
}

auto Compute::Fault(VkDeviceAddress bad, VkDeviceAddress out, double wait_ms, double& wait) -> VkResult {
  VkResult r = Begin();
  if (r != VK_SUCCESS) return r;
  vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_COMPUTE, fault_);
  const Push p{bad, out};
  vkCmdPushConstants(cb_, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
  vkCmdDispatch(cb_, 1, 1, 1);
  r = vkEndCommandBuffer(cb_);
  if (r != VK_SUCCESS) return r;
  return SubmitAndWait(wait_ms, &wait);
}

auto Compute::Ping(double wait_ms) -> VkResult {
  VkResult r = Begin();
  if (r != VK_SUCCESS) return r;
  r = vkEndCommandBuffer(cb_);
  if (r != VK_SUCCESS) return r;
  return SubmitAndWait(wait_ms, nullptr);
}

auto Compute::FaultReport() -> std::string {
  if (!ctx_.device_fault_enabled) return "VK_EXT_device_fault not enabled";
  auto get = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(
      vkGetDeviceProcAddr(dev_, "vkGetDeviceFaultInfoEXT"));
  if (get == nullptr) return "vkGetDeviceFaultInfoEXT absent";
  VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
  VkResult               r = get(dev_, &counts, nullptr);
  std::ostringstream     text;
  text << "counts VkResult " << static_cast<int>(r) << " address_infos=" << counts.addressInfoCount
       << " vendor_infos=" << counts.vendorInfoCount << " vendor_binary=" << counts.vendorBinarySize;
  if (r != VK_SUCCESS && r != VK_INCOMPLETE) return text.str();
  std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
  std::vector<VkDeviceFaultVendorInfoEXT>  vendors(counts.vendorInfoCount);
  VkDeviceFaultInfoEXT                     info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
  info.pAddressInfos   = addresses.empty() ? nullptr : addresses.data();
  info.pVendorInfos    = vendors.empty() ? nullptr : vendors.data();
  counts.vendorBinarySize = 0;
  r = get(dev_, &counts, &info);
  text << " info VkResult " << static_cast<int>(r) << " description='" << info.description << "'";
  for (const auto& a : addresses) {
    text << " address(type=" << static_cast<int>(a.addressType) << " 0x" << std::hex
         << a.reportedAddress << " precision=0x" << a.addressPrecision << std::dec << ")";
  }
  for (const auto& v : vendors) {
    text << " vendor('" << v.description << "' code=" << v.vendorFaultCode
         << " data=" << v.vendorFaultData << ")";
  }
  return text.str();
}

}  // namespace vx5
