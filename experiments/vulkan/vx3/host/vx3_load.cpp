//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX3 experiment: the load runner. Experiment code: it does not merge.
#include "vx3_load.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>

#include "edit/runtime/local_tone_mapping.hpp"
#include "vx2_common.hpp"
#include "vx3_film_tables.hpp"

namespace vx3 {

using vx2::Log;
using vx2::NowNs;
using vx2::VkCheck;

namespace {

constexpr std::uint32_t kGroup      = 16;
constexpr std::uint32_t kMaxQueries = 4096;
constexpr int           kSlots      = 3;

auto ReadSpirv(const std::string& path) -> std::vector<std::uint32_t> {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) throw std::runtime_error("cannot open " + path);
  const auto size = static_cast<std::size_t>(file.tellg());
  std::vector<std::uint32_t> words(size / 4);
  file.seekg(0);
  file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(size));
  return words;
}

template <typename T>
void SetPush(Op& op, const T& value) {
  static_assert(sizeof(T) <= 128);
  std::memcpy(op.push.data(), &value, sizeof(T));
  op.push_size = sizeof(T);
}

auto Groups(std::uint32_t threads) -> std::uint32_t { return (threads + kGroup - 1) / kGroup; }

}  // namespace

Gpu::Gpu(VulkanContext& ctx, const std::string& spv_dir)
    : ctx_(ctx), dev_(ctx.pipe_device), spv_dir_(spv_dir) {
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(ctx_.physical, &props);
  timestamp_ns_ = props.limits.timestampPeriod;
  SetupLayout();
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool.queueFamilyIndex = ctx_.family;
  VkCheck(vkCreateCommandPool(dev_, &pool, nullptr, &pool_), "vkCreateCommandPool");
  VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qp.queryType  = VK_QUERY_TYPE_TIMESTAMP;
  qp.queryCount = kMaxQueries;
  VkCheck(vkCreateQueryPool(dev_, &qp, nullptr, &queries_), "vkCreateQueryPool");
  query_count_ = kMaxQueries;
  slots_.resize(kSlots);
  for (auto& s : slots_) {
    VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    a.commandPool        = pool_;
    a.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    a.commandBufferCount = 1;
    VkCheck(vkAllocateCommandBuffers(dev_, &a, &s.cb), "vkAllocateCommandBuffers");
    VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkCheck(vkCreateFence(dev_, &f, nullptr, &s.fence), "vkCreateFence");
  }
}

Gpu::~Gpu() {
  vkDeviceWaitIdle(dev_);
  for (auto& s : slots_) vkDestroyFence(dev_, s.fence, nullptr);
  vkDestroyQueryPool(dev_, queries_, nullptr);
  vkDestroyCommandPool(dev_, pool_, nullptr);
  for (auto p : pipelines_) vkDestroyPipeline(dev_, p, nullptr);
  for (auto m : modules_) vkDestroyShaderModule(dev_, m, nullptr);
  vkDestroyPipelineLayout(dev_, layout_, nullptr);
  vkDestroyDescriptorSetLayout(dev_, set_layout_, nullptr);
}

void Gpu::SetupLayout() {
  const VkDescriptorSetLayoutBinding bindings[2] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dsl.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
  dsl.bindingCount = 2;
  dsl.pBindings    = bindings;
  VkCheck(vkCreateDescriptorSetLayout(dev_, &dsl, nullptr, &set_layout_), "vkCreateDescriptorSetLayout");
  const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.setLayoutCount         = 1;
  pl.pSetLayouts            = &set_layout_;
  pl.pushConstantRangeCount = 1;
  pl.pPushConstantRanges    = &range;
  VkCheck(vkCreatePipelineLayout(dev_, &pl, nullptr, &layout_), "vkCreatePipelineLayout");
}

auto Gpu::Kernel(const std::string& module, const std::string& entry) -> int {
  for (std::size_t i = 0; i < kernel_names_.size(); ++i) {
    if (kernel_names_[i] == module + ":" + entry) return static_cast<int>(i);
  }
  VkShaderModule shader = VK_NULL_HANDLE;
  for (std::size_t i = 0; i < module_names_.size(); ++i) {
    if (module_names_[i] == module) shader = modules_[i];
  }
  if (shader == VK_NULL_HANDLE) {
    const auto words = ReadSpirv(spv_dir_ + "/" + module + ".spv");
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = words.size() * 4;
    sm.pCode    = words.data();
    VkCheck(vkCreateShaderModule(dev_, &sm, nullptr, &shader), "vkCreateShaderModule");
    modules_.push_back(shader);
    module_names_.push_back(module);
  }
  VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  // Bands of an op are dispatches with a base group (vkCmdDispatchBase).
  cp.flags  = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
  cp.stage  = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
               VK_SHADER_STAGE_COMPUTE_BIT, shader, entry.c_str(), nullptr};
  cp.layout = layout_;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkCheck(vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &cp, nullptr, &pipeline),
          ("vkCreateComputePipelines " + entry).c_str());
  pipelines_.push_back(pipeline);
  kernel_names_.push_back(module + ":" + entry);
  return static_cast<int>(pipelines_.size() - 1);
}

auto Gpu::CreateBuffer(VkDeviceSize size) -> Buffer {
  Buffer             b;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size  = std::max<VkDeviceSize>(size, 256);
  info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkCheck(vkCreateBuffer(dev_, &info, nullptr, &b.buffer), "vkCreateBuffer");
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(dev_, b.buffer, &req);
  VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
  a.allocationSize  = req.size;
  a.memoryTypeIndex = ctx_.MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkCheck(vkAllocateMemory(dev_, &a, nullptr, &b.memory),
          ("vkAllocateMemory " + std::to_string(req.size >> 20) + " MiB").c_str());
  VkCheck(vkBindBufferMemory(dev_, b.buffer, b.memory, 0), "vkBindBufferMemory");
  VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  ai.buffer = b.buffer;
  b.address = vkGetBufferDeviceAddress(dev_, &ai);
  b.size    = info.size;
  bytes_ += req.size;
  return b;
}

auto Gpu::CreateImage(std::uint32_t width, std::uint32_t height) -> Image {
  Image             im;
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType   = VK_IMAGE_TYPE_2D;
  ii.format      = VK_FORMAT_R32G32B32A32_SFLOAT;
  ii.extent      = {width, height, 1};
  ii.mipLevels   = 1;
  ii.arrayLayers = 1;
  ii.samples     = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
  ii.usage       = VK_IMAGE_USAGE_STORAGE_BIT;
  VkCheck(vkCreateImage(dev_, &ii, nullptr, &im.image), "vkCreateImage");
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(dev_, im.image, &req);
  VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  a.allocationSize  = req.size;
  a.memoryTypeIndex = ctx_.MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkCheck(vkAllocateMemory(dev_, &a, nullptr, &im.memory), "vkAllocateMemory (image)");
  VkCheck(vkBindImageMemory(dev_, im.image, im.memory, 0), "vkBindImageMemory");
  VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  v.image            = im.image;
  v.viewType         = VK_IMAGE_VIEW_TYPE_2D;
  v.format           = ii.format;
  v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkCheck(vkCreateImageView(dev_, &v, nullptr, &im.view), "vkCreateImageView");
  bytes_ += req.size;

  // UNDEFINED -> GENERAL, once.
  auto& s = slots_[0];
  VkCheck(vkResetCommandBuffer(s.cb, 0), "vkResetCommandBuffer");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VkCheck(vkBeginCommandBuffer(s.cb, &begin), "vkBeginCommandBuffer");
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image               = im.image;
  b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &b);
  VkCheck(vkEndCommandBuffer(s.cb), "vkEndCommandBuffer");
  VkCheck(SubmitLocked(s.cb, s.fence), "vkQueueSubmit (layout)");
  VkCheck(vkWaitForFences(dev_, 1, &s.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences (layout)");
  vkResetFences(dev_, 1, &s.fence);
  return im;
}

void Gpu::Upload(const Buffer& dst, const void* data, std::size_t bytes) {
  VkBuffer           staging = VK_NULL_HANDLE;
  VkDeviceMemory     memory  = VK_NULL_HANDLE;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size  = bytes;
  info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VkCheck(vkCreateBuffer(dev_, &info, nullptr, &staging), "vkCreateBuffer (staging)");
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(dev_, staging, &req);
  VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  a.allocationSize  = req.size;
  a.memoryTypeIndex = ctx_.MemoryType(
      req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkCheck(vkAllocateMemory(dev_, &a, nullptr, &memory), "vkAllocateMemory (staging)");
  VkCheck(vkBindBufferMemory(dev_, staging, memory, 0), "vkBindBufferMemory (staging)");
  void* map = nullptr;
  VkCheck(vkMapMemory(dev_, memory, 0, bytes, 0, &map), "vkMapMemory");
  std::memcpy(map, data, bytes);
  vkUnmapMemory(dev_, memory);
  auto& s = slots_[0];
  VkCheck(vkResetCommandBuffer(s.cb, 0), "vkResetCommandBuffer");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VkCheck(vkBeginCommandBuffer(s.cb, &begin), "vkBeginCommandBuffer");
  const VkBufferCopy copy{0, 0, bytes};
  vkCmdCopyBuffer(s.cb, staging, dst.buffer, 1, &copy);
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                       1, &mb, 0, nullptr, 0, nullptr);
  VkCheck(vkEndCommandBuffer(s.cb), "vkEndCommandBuffer");
  VkCheck(SubmitLocked(s.cb, s.fence), "vkQueueSubmit (upload)");
  VkCheck(vkWaitForFences(dev_, 1, &s.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences (upload)");
  vkResetFences(dev_, 1, &s.fence);
  vkDestroyBuffer(dev_, staging, nullptr);
  vkFreeMemory(dev_, memory, nullptr);
}

void Gpu::Destroy(Buffer& buffer) {
  if (buffer.buffer != VK_NULL_HANDLE) vkDestroyBuffer(dev_, buffer.buffer, nullptr);
  if (buffer.memory != VK_NULL_HANDLE) vkFreeMemory(dev_, buffer.memory, nullptr);
  buffer = {};
}

void Gpu::Destroy(Image& image) {
  if (image.view != VK_NULL_HANDLE) vkDestroyImageView(dev_, image.view, nullptr);
  if (image.image != VK_NULL_HANDLE) vkDestroyImage(dev_, image.image, nullptr);
  if (image.memory != VK_NULL_HANDLE) vkFreeMemory(dev_, image.memory, nullptr);
  image = {};
}

auto Gpu::SubmitLocked(VkCommandBuffer cb, VkFence fence) -> VkResult {
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers    = &cb;
  // The pipeline queue has two user threads in VX3 (the preview producer and this load); a VkQueue
  // needs external synchronization.
  std::lock_guard<std::mutex> lock(ctx_.pipeline_submit_mutex);
  return vkQueueSubmit(ctx_.pipeline_queue, 1, &si, fence);
}

void Gpu::Record(VkCommandBuffer cb, const std::vector<Op>& ops, const Chunk& chunk,
                 std::uint32_t query0) {
  VkCheck(vkResetCommandBuffer(cb, 0), "vkResetCommandBuffer");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VkCheck(vkBeginCommandBuffer(cb, &begin), "vkBeginCommandBuffer");
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  auto barrier     = [&] {
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
  };
  const bool per_band_stamps = query0 == UINT32_MAX - 1;  // calibration form
  const std::uint32_t q0     = per_band_stamps ? 0 : query0;
  const std::uint32_t stamps = per_band_stamps ? static_cast<std::uint32_t>(chunk.bands.size()) + 1 : 2;
  vkCmdResetQueryPool(cb, queries_, q0, stamps);
  // The first barrier covers the work of the earlier submissions on this queue.
  barrier();
  vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, q0);
  int last_op = -1, last_kernel = -1;
  for (std::size_t i = 0; i < chunk.bands.size(); ++i) {
    const auto& band = chunk.bands[i];
    const Op&   op   = ops[static_cast<std::size_t>(band.op)];
    if (last_op >= 0 && band.op != last_op) barrier();
    if (op.kernel != last_kernel) {
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[op.kernel]);
      last_kernel = op.kernel;
    }
    if (op.image0 != VK_NULL_HANDLE || op.image1 != VK_NULL_HANDLE) {
      VkDescriptorImageInfo info[2] = {{VK_NULL_HANDLE, op.image0, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, op.image1, VK_IMAGE_LAYOUT_GENERAL}};
      VkWriteDescriptorSet  writes[2]{};
      std::uint32_t         n = 0;
      for (std::uint32_t k = 0; k < 2; ++k) {
        if (info[k].imageView == VK_NULL_HANDLE) continue;
        writes[n].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[n].dstBinding      = k;
        writes[n].descriptorCount = 1;
        writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[n].pImageInfo      = &info[k];
        ++n;
      }
      ctx_.push_descriptor(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, n, writes);
    }
    vkCmdPushConstants(cb, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, op.push_size, op.push.data());
    const std::uint32_t g0 = band.row0 / kGroup;
    const std::uint32_t g1 = Groups(band.row1);
    vkCmdDispatchBase(cb, 0, g0, 0, Groups(op.width), g1 - g0, 1);
    last_op = band.op;
    if (per_band_stamps) {
      barrier();
      vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_,
                          static_cast<std::uint32_t>(i) + 1);
    }
  }
  if (!per_band_stamps) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, q0 + 1);
  VkCheck(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
}

void Gpu::Calibrate(std::vector<Op>& ops, int runs) {
  if (ops.size() + 1 > query_count_) throw std::runtime_error("too many ops to calibrate");
  Chunk all;
  for (std::size_t i = 0; i < ops.size(); ++i) {
    all.bands.push_back({static_cast<int>(i), 0, ops[i].height});
  }
  std::vector<double> best(ops.size(), 1e30);
  for (int r = 0; r < runs; ++r) {
    auto& s = slots_[0];
    Record(s.cb, ops, all, UINT32_MAX - 1);
    VkCheck(SubmitLocked(s.cb, s.fence), "vkQueueSubmit (calibrate)");
    VkCheck(vkWaitForFences(dev_, 1, &s.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences (calibrate)");
    vkResetFences(dev_, 1, &s.fence);
    std::vector<std::uint64_t> t(ops.size() + 1);
    VkCheck(vkGetQueryPoolResults(dev_, queries_, 0, static_cast<std::uint32_t>(t.size()),
                                  t.size() * 8, t.data(), 8,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
            "vkGetQueryPoolResults (calibrate)");
    for (std::size_t i = 0; i < ops.size(); ++i) {
      best[i] = std::min(best[i], static_cast<double>(t[i + 1] - t[i]) * timestamp_ns_ * 1e-6);
    }
  }
  for (std::size_t i = 0; i < ops.size(); ++i) ops[i].ms = best[i];
}

void Gpu::RunOnce(const std::vector<Op>& ops) {
  Chunk all;
  for (std::size_t i = 0; i < ops.size(); ++i) all.bands.push_back({static_cast<int>(i), 0, ops[i].height});
  auto& s = slots_[0];
  Record(s.cb, ops, all, 0);
  VkCheck(SubmitLocked(s.cb, s.fence), "vkQueueSubmit (once)");
  VkCheck(vkWaitForFences(dev_, 1, &s.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences (once)");
  vkResetFences(dev_, 1, &s.fence);
}

auto Gpu::Execute(const std::vector<Op>& ops, const std::vector<Chunk>& chunks, int inflight,
                  std::int64_t wait_limit_ns) -> RunRecord {
  RunRecord run;
  run.chunks.resize(chunks.size());
  inflight = std::clamp(inflight, 1, kSlots);
  std::vector<int> slot_chunk(slots_.size(), -1);
  auto finish = [&](int slot) -> bool {
    const int c = slot_chunk[static_cast<std::size_t>(slot)];
    if (c < 0) return true;
    auto&              s     = slots_[static_cast<std::size_t>(slot)];
    const std::int64_t start = NowNs();
    while (true) {
      const VkResult r = vkWaitForFences(dev_, 1, &s.fence, VK_TRUE, 100'000'000ull);
      if (r == VK_SUCCESS) break;
      if (r == VK_TIMEOUT) {
        if (NowNs() - start > wait_limit_ns) {
          run.error = "fence wait above the limit";
          return false;
        }
        continue;
      }
      run.device_lost = r == VK_ERROR_DEVICE_LOST;
      run.error       = "vkWaitForFences VkResult " + std::to_string(static_cast<int>(r));
      return false;
    }
    run.chunks[static_cast<std::size_t>(c)].complete_ns = NowNs();
    vkResetFences(dev_, 1, &s.fence);
    std::uint64_t t[2]{};
    if (vkGetQueryPoolResults(dev_, queries_, static_cast<std::uint32_t>(slot) * 2, 2, sizeof(t), t,
                              8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      run.chunks[static_cast<std::size_t>(c)].gpu_ms =
          static_cast<double>(t[1] - t[0]) * timestamp_ns_ * 1e-6;
    }
    slot_chunk[static_cast<std::size_t>(slot)] = -1;
    return true;
  };
  run.begin_ns = NowNs();
  for (std::size_t c = 0; c < chunks.size(); ++c) {
    const int slot = static_cast<int>(c % static_cast<std::size_t>(inflight));
    if (!finish(slot)) {
      run.end_ns = NowNs();
      return run;
    }
    auto& s = slots_[static_cast<std::size_t>(slot)];
    Record(s.cb, ops, chunks[c], static_cast<std::uint32_t>(slot) * 2);
    run.chunks[c].submit_ns = NowNs();
    const VkResult r        = SubmitLocked(s.cb, s.fence);
    if (r != VK_SUCCESS) {
      run.device_lost = r == VK_ERROR_DEVICE_LOST;
      run.error       = "vkQueueSubmit VkResult " + std::to_string(static_cast<int>(r));
      run.end_ns      = NowNs();
      return run;
    }
    slot_chunk[static_cast<std::size_t>(slot)] = static_cast<int>(c);
  }
  for (int slot = 0; slot < inflight; ++slot) {
    if (!finish(slot)) break;
  }
  run.end_ns = NowNs();
  return run;
}

auto Plan(const std::vector<Op>& ops, const std::string& mode, double budget_ms)
    -> std::vector<Chunk> {
  std::vector<Chunk> chunks;
  if (mode == "whole") {
    Chunk all;
    for (std::size_t i = 0; i < ops.size(); ++i) {
      all.bands.push_back({static_cast<int>(i), 0, ops[i].height});
      all.estimate_ms += ops[i].ms;
    }
    chunks.push_back(all);
    return chunks;
  }
  if (mode == "op") {
    for (std::size_t i = 0; i < ops.size(); ++i) {
      chunks.push_back({{{static_cast<int>(i), 0, ops[i].height}}, ops[i].ms});
    }
    return chunks;
  }
  if (mode != "budget" || budget_ms <= 0) throw std::runtime_error("split must be whole, op, or budget");
  Chunk current;
  for (std::size_t i = 0; i < ops.size(); ++i) {
    const Op&           op         = ops[i];
    const std::uint32_t group_rows = Groups(op.height);
    const double        per_group  = op.ms / std::max<std::uint32_t>(1, group_rows);
    std::uint32_t       g          = 0;
    while (g < group_rows) {
      const double room = budget_ms - current.estimate_ms;
      std::uint32_t fit = per_group > 0 ? static_cast<std::uint32_t>(std::floor(room / per_group))
                                        : group_rows;
      fit = std::min(fit, group_rows - g);
      if (fit == 0) {
        if (current.bands.empty()) {
          fit = 1;  // one row of groups is above the budget: it gets a submission of its own
        } else {
          chunks.push_back(current);
          current = {};
          continue;
        }
      }
      current.bands.push_back({static_cast<int>(i), g * kGroup,
                               std::min(op.height, (g + fit) * kGroup)});
      current.estimate_ms += per_group * fit;
      g += fit;
    }
  }
  if (!current.bands.empty()) chunks.push_back(current);
  return chunks;
}

// --- render -------------------------------------------------------------------------------------

namespace {

struct LlfPush {
  VkDeviceAddress src = 0, dst = 0, lo = 0, lo_coarse = 0, hi = 0, hi_coarse = 0;
  std::int32_t    width = 0, height = 0, coarse_width = 0, coarse_height = 0;
  std::int32_t    input_width = 0, input_height = 0;
  float           gamma = 0, target = 0, beta = 1, alpha = 1, sigma = 0, gamma_lo = 0, gamma_hi = 0;
  std::int32_t    first = 0, last = 0, top = 0;
};
static_assert(sizeof(LlfPush) == 112);

struct FilmPush {
  VkDeviceAddress data = 0, values = 0, q = 0, a = 0, print_rgba = 0;
  std::uint32_t   params_offset = 0, q_offset = 0, a_offset = 0;
  std::int32_t    width = 0, height = 0, inhibitor_blur = 0;
};
static_assert(sizeof(FilmPush) == 64);

struct BlurPush {
  VkDeviceAddress src = 0, dst = 0, weights = 0;
  std::int32_t    width = 0, height = 0, radius = 0;
  float           weight = 1;
  std::int32_t    accumulate = 0;
};
static_assert(sizeof(BlurPush) == 48);

struct SpinPush {
  VkDeviceAddress out = 0;
  std::uint32_t   iterations = 0, width = 0, height = 0;
};

}  // namespace

void Render::Build(Gpu& gpu, std::uint32_t w, std::uint32_t h, int blur_radius, int blur_pairs) {
  namespace ltm = alcedo::local_tone_mapping;
  width   = w;
  height  = h;
  scene_a = gpu.CreateImage(w, h);
  scene_b = gpu.CreateImage(w, h);
  const VkDeviceSize plane3 = VkDeviceSize(w) * h * 3 * sizeof(float);
  q   = gpu.CreateBuffer(plane3);
  a   = gpu.CreateBuffer(plane3);
  mid = gpu.CreateBuffer(plane3);

  const int k_init    = gpu.Kernel("vx3_llf", "vx3_init_scene");
  const int k_extract = gpu.Kernel("vx3_llf", "llf_extract");
  const int k_down    = gpu.Kernel("vx3_llf", "llf_pyr_down");
  const int k_remap   = gpu.Kernel("vx3_llf", "llf_remap");
  const int k_select  = gpu.Kernel("vx3_llf", "llf_select");
  const int k_collapse = gpu.Kernel("vx3_llf", "llf_collapse");
  const int k_apply   = gpu.Kernel("vx3_llf", "llf_apply");
  const int k_expo    = gpu.Kernel("film_print", "film_print_exposure");
  const int k_first   = gpu.Kernel("film_print", "film_print_first_amounts");
  const int k_display = gpu.Kernel("film_print", "film_print_display");
  const int k_blur_h  = gpu.Kernel("vx3_blur", "film_blur_horizontal");
  const int k_blur_v  = gpu.Kernel("vx3_blur", "film_blur_vertical");

  {
    Op op;
    op.name = "init_scene";
    op.cls  = "point";
    op.kernel = k_init;
    LlfPush p;
    p.width = static_cast<int>(w);
    p.height = static_cast<int>(h);
    SetPush(op, p);
    op.image1 = scene_a.view;
    op.width  = w;
    op.height = h;
    init.push_back(op);
  }

  // Local Tone: canonical planes at 2048 pixels on the long edge, the pyramid of local_tone_plan.
  const auto dims = ltm::ComputeMaskDimensions(static_cast<int>(w), static_cast<int>(h),
                                               ltm::kReferenceMaskMaxLongEdge);
  llf_width   = static_cast<std::uint32_t>(dims.width);
  llf_height  = static_cast<std::uint32_t>(dims.height);
  llf_levels  = ltm::ComputeLevelCount(dims.width, dims.height, ltm::kPyramidRadius);
  std::vector<int> lw(llf_levels), lh(llf_levels);
  lw[0] = dims.width;
  lh[0] = dims.height;
  for (int l = 1; l < llf_levels; ++l) {
    lw[l] = std::max(1, (lw[l - 1] + 1) / 2);
    lh[l] = std::max(1, (lh[l - 1] + 1) / 2);
  }
  // Shadows +60, Highlights -60 (amounts of the clamped sliders).
  const float shadow    = 60.0f * ltm::kHighlightStrengthScale / 80.0f;
  const float highlight = -60.0f * ltm::kHighlightStrengthScale / 100.0f;
  const float sigma     = ltm::SigmaR(shadow, highlight);
  const auto  samples   = ltm::BuildSamples(shadow, highlight);
  llf_samples           = static_cast<int>(samples.size());

  using Pyramid = std::vector<Buffer>;
  auto make_pyramid = [&] {
    Pyramid p;
    for (int l = 0; l < llf_levels; ++l) {
      p.push_back(gpu.CreateBuffer(VkDeviceSize(lw[l]) * lh[l] * sizeof(float)));
      planes.push_back(p.back());
    }
    return p;
  };
  Pyramid source = make_pyramid(), remap_a = make_pyramid(), remap_b = make_pyramid(),
          result = make_pyramid();
  auto small_op = [&](const char* name, int kernel, const LlfPush& p, std::uint32_t ow,
                      std::uint32_t oh) {
    Op op;
    op.name   = name;
    op.cls    = "small";
    op.kernel = kernel;
    SetPush(op, p);
    op.width  = ow;
    op.height = oh;
    ops.push_back(op);
  };
  {
    LlfPush p;
    p.dst          = source[0].address;
    p.width        = lw[0];
    p.height       = lh[0];
    p.input_width  = static_cast<int>(w);
    p.input_height = static_cast<int>(h);
    small_op("llf_extract", k_extract, p, lw[0], lh[0]);
    ops.back().image0 = scene_a.view;
  }
  auto pyramid_down = [&](Pyramid& levels) {
    for (int l = 1; l < llf_levels; ++l) {
      LlfPush p;
      p.src          = levels[l - 1].address;
      p.dst          = levels[l].address;
      p.width        = lw[l];
      p.height       = lh[l];
      p.input_width  = lw[l - 1];
      p.input_height = lh[l - 1];
      small_op("llf_pyr_down", k_down, p, lw[l], lh[l]);
    }
  };
  pyramid_down(source);
  auto build_remap = [&](const ltm::LlfSample& s, Pyramid& levels) {
    LlfPush p;
    p.src    = source[0].address;
    p.dst    = levels[0].address;
    p.width  = lw[0];
    p.height = lh[0];
    p.gamma  = s.gamma;
    p.target = s.target;
    p.beta   = s.beta;
    p.alpha  = s.alpha;
    p.sigma  = sigma;
    small_op("llf_remap", k_remap, p, lw[0], lh[0]);
    pyramid_down(levels);
  };
  build_remap(samples[0], remap_a);
  build_remap(samples[1], remap_b);
  for (std::size_t pair = 0; pair + 1 < samples.size(); ++pair) {
    for (int l = 0; l < llf_levels; ++l) {
      const bool top = l + 1 == llf_levels;
      LlfPush    p;
      p.src           = source[l].address;
      p.lo            = remap_a[l].address;
      p.lo_coarse     = (top ? remap_a[l] : remap_a[l + 1]).address;
      p.hi            = remap_b[l].address;
      p.hi_coarse     = (top ? remap_b[l] : remap_b[l + 1]).address;
      p.dst           = result[l].address;
      p.width         = lw[l];
      p.height        = lh[l];
      p.coarse_width  = top ? 1 : lw[l + 1];
      p.coarse_height = top ? 1 : lh[l + 1];
      p.gamma_lo      = samples[pair].gamma;
      p.gamma_hi      = samples[pair + 1].gamma;
      p.first         = pair == 0 ? 1 : 0;
      p.last          = pair + 2 == samples.size() ? 1 : 0;
      p.top           = top ? 1 : 0;
      small_op("llf_select", k_select, p, lw[l], lh[l]);
    }
    if (pair + 2 < samples.size()) {
      std::swap(remap_a, remap_b);
      build_remap(samples[pair + 2], remap_b);
    }
  }
  for (int l = llf_levels - 2; l >= 0; --l) {
    LlfPush p;
    p.src           = result[l].address;
    p.lo            = result[l + 1].address;
    p.dst           = remap_a[l].address;
    p.width         = lw[l];
    p.height        = lh[l];
    p.coarse_width  = lw[l + 1];
    p.coarse_height = lh[l + 1];
    small_op("llf_collapse", k_collapse, p, lw[l], lh[l]);
    std::swap(result[l], remap_a[l]);
  }
  {
    Op      op;
    op.name   = "llf_apply";
    op.cls    = "point";
    op.kernel = k_apply;
    LlfPush p;
    p.src           = source[0].address;
    p.lo            = result[0].address;
    p.width         = static_cast<int>(w);
    p.height        = static_cast<int>(h);
    p.coarse_width  = lw[0];
    p.coarse_height = lh[0];
    SetPush(op, p);
    op.image0 = scene_a.view;
    op.image1 = scene_b.view;
    op.width  = w;
    op.height = h;
    ops.push_back(op);
  }

  // Film print: FP1, the spread blur, FP4 with the inhibitor blur, a halation blur, FP6.
  const auto tables = MakeFilmTables();
  film_data   = gpu.CreateBuffer(sizeof(FilmKernelParams));
  film_values = gpu.CreateBuffer(tables.values.size() * 4);
  film_print  = gpu.CreateBuffer(tables.print_rgba.size() * 4);
  gpu.Upload(film_data, &tables.params, sizeof(FilmKernelParams));
  gpu.Upload(film_values, tables.values.data(), tables.values.size() * 4);
  gpu.Upload(film_print, tables.print_rgba.data(), tables.print_rgba.size() * 4);
  std::vector<float> weights(3 * 129, 0.0f);
  {
    const double s = std::max(1.0, blur_radius / 3.0);
    for (int c = 0; c < 3; ++c) {
      double sum = 0;
      for (int t = 0; t <= blur_radius; ++t) sum += (t == 0 ? 1.0 : 2.0) * std::exp(-0.5 * t * t / (s * s));
      for (int t = 0; t <= blur_radius; ++t) {
        weights[c * 129 + t] = static_cast<float>(std::exp(-0.5 * t * t / (s * s)) / sum);
      }
    }
  }
  blur_weights = gpu.CreateBuffer(weights.size() * 4);
  gpu.Upload(blur_weights, weights.data(), weights.size() * 4);

  FilmPush fp;
  fp.data           = film_data.address;
  fp.values         = film_values.address;
  fp.q              = q.address;
  fp.a              = a.address;
  fp.print_rgba     = film_print.address;
  fp.width          = static_cast<int>(w);
  fp.height         = static_cast<int>(h);
  fp.inhibitor_blur = 1;
  auto film_op = [&](const char* name, int kernel, VkImageView i0, VkImageView i1) {
    Op op;
    op.name   = name;
    op.cls    = "point";
    op.kernel = kernel;
    SetPush(op, fp);
    op.image0 = i0;
    op.image1 = i1;
    op.width  = w;
    op.height = h;
    ops.push_back(op);
  };
  auto blur_pair = [&](const char* name, const Buffer& plane, float weight, int accumulate) {
    BlurPush b;
    b.src     = plane.address;
    b.dst     = mid.address;
    b.weights = blur_weights.address;
    b.width   = static_cast<int>(w);
    b.height  = static_cast<int>(h);
    b.radius  = blur_radius;
    Op op;
    op.name   = std::string(name) + "_h";
    op.cls    = "neighbourhood";
    op.kernel = k_blur_h;
    SetPush(op, b);
    op.width  = w;
    op.height = h;
    ops.push_back(op);
    b.src        = mid.address;
    b.dst        = plane.address;
    b.weight     = weight;
    b.accumulate = accumulate;
    op.name      = std::string(name) + "_v";
    op.kernel    = k_blur_v;
    SetPush(op, b);
    ops.push_back(op);
  };
  film_op("film_exposure", k_expo, scene_b.view, VK_NULL_HANDLE);
  if (blur_pairs >= 1) blur_pair("film_spread", q, 1.0f, 0);
  film_op("film_first_amounts", k_first, VK_NULL_HANDLE, VK_NULL_HANDLE);
  if (blur_pairs >= 2) blur_pair("film_inhibitor", a, 1.0f, 0);
  if (blur_pairs >= 3) blur_pair("film_halation", a, 0.0f, 1);
  film_op("film_display", k_display, scene_b.view, scene_a.view);
}

void Render::Release(Gpu& gpu) {
  gpu.Destroy(scene_a);
  gpu.Destroy(scene_b);
  for (Buffer* b : {&q, &a, &mid, &film_data, &film_values, &film_print, &blur_weights}) gpu.Destroy(*b);
  for (auto& p : planes) gpu.Destroy(p);
  planes.clear();
}

void Spin::Build(Gpu& gpu, const std::string& form, double target_ms, int count) {
  out = gpu.CreateBuffer(1024 * 1024 * 4);
  const int kernel = gpu.Kernel("vx3_spin", "vx3_spin");
  // wide: 8192 x 4096 threads; long: one group of 16 x 16 threads.
  const bool         one_group = form == "long";
  const std::uint32_t gw = one_group ? 16 : 8192, gh = one_group ? 16 : 4096;
  auto make = [&](std::uint32_t iterations) {
    Op op;
    op.name   = "spin_" + form;
    op.cls    = "spin";
    op.kernel = kernel;
    SpinPush p;
    p.out        = out.address;
    p.iterations = iterations;
    p.width      = gw;
    p.height     = gh;
    SetPush(op, p);
    op.width  = gw;
    op.height = gh;
    return op;
  };
  // Two calibration steps: the loop time is linear in the count of iterations.
  std::uint32_t iterations = one_group ? 200000u : 64u;
  const double  one_ms     = form == "many" ? target_ms / std::max(1, count) : target_ms;
  for (int step = 0; step < 3; ++step) {
    std::vector<Op> probe{make(iterations)};
    gpu.Calibrate(probe, 1);
    const double ms = std::max(probe[0].ms, 1e-3);
    Log("SPIN calibrate form=%s iterations=%u ms=%.3f", form.c_str(), iterations, ms);
    const double scale = std::min(one_ms / ms, step == 2 ? 1e9 : 64.0);
    if (step == 2 || std::abs(scale - 1.0) < 0.02) {
      iterations = static_cast<std::uint32_t>(std::clamp(iterations * scale, 1.0, 4.0e9));
      break;
    }
    iterations = static_cast<std::uint32_t>(std::clamp(iterations * scale, 1.0, 4.0e9));
  }
  const int n = form == "many" ? std::max(1, count) : 1;
  for (int i = 0; i < n; ++i) ops.push_back(make(iterations));
  Log("SPIN form=%s dispatches=%d iterations=%u expected_ms_each=%.1f", form.c_str(), n, iterations,
      one_ms);
}

void Spin::Release(Gpu& gpu) { gpu.Destroy(out); }

}  // namespace vx3
