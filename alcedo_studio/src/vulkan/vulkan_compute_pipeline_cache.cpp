//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "vulkan/vulkan_compute_pipeline_cache.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "utils/logging/log.hpp"
#include "vulkan/vulkan_api_counters.hpp"
#include "vulkan/vulkan_result.hpp"

namespace alcedo::vulkan {
namespace {

auto IsImageDescriptorType(VkDescriptorType type) -> bool {
  return type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
         type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

/// Checks the descriptor and gives its bindings sorted by binding number.
auto CheckedBindings(const VulkanKernelDescriptor& descriptor, uint32_t max_push_constant_bytes)
    -> std::vector<VulkanKernelBinding> {
  auto bindings = descriptor.bindings;
  std::sort(bindings.begin(), bindings.end(),
            [](const auto& a, const auto& b) { return a.binding < b.binding; });
  for (std::size_t i = 0; i < bindings.size(); ++i) {
    if (!IsImageDescriptorType(bindings[i].type)) {
      throw std::invalid_argument("Vulkan kernel " + descriptor.kernel + ": binding " +
                                  std::to_string(bindings[i].binding) +
                                  " is not an image; a buffer is a device address in the push "
                                  "constants.");
    }
    if (i > 0 && bindings[i].binding == bindings[i - 1].binding) {
      throw std::invalid_argument("Vulkan kernel " + descriptor.kernel + ": binding " +
                                  std::to_string(bindings[i].binding) + " is given two times.");
    }
  }
  if (descriptor.push_constant_bytes % 4 != 0 ||
      descriptor.push_constant_bytes > max_push_constant_bytes) {
    throw std::invalid_argument("Vulkan kernel " + descriptor.kernel + ": push constant size " +
                                std::to_string(descriptor.push_constant_bytes) +
                                " is not a multiple of 4 or is above the device limit " +
                                std::to_string(max_push_constant_bytes) + ".");
  }
  return bindings;
}

auto SortedSpecialization(const VulkanKernelDescriptor& descriptor)
    -> std::vector<VulkanSpecializationValue> {
  auto values = descriptor.specialization;
  std::sort(values.begin(), values.end());
  for (std::size_t i = 1; i < values.size(); ++i) {
    if (values[i].constant_id == values[i - 1].constant_id) {
      throw std::invalid_argument("Vulkan kernel " + descriptor.kernel + ": constant id " +
                                  std::to_string(values[i].constant_id) + " is given two times.");
    }
  }
  return values;
}

}  // namespace

VulkanComputePipelineCache::VulkanComputePipelineCache(VulkanComputePipelineCacheDevice device,
                                                       VulkanShaderModuleRegistry&      modules,
                                                       std::filesystem::path            cache_file)
    : device_(device), modules_(&modules), cache_file_(std::move(cache_file)) {
  VulkanStoredPipelineCacheData stored;
  if (!cache_file_.empty()) {
    stored              = ReadStoredPipelineCacheData(cache_file_, device_.identity);
    stored_data_status_ = stored.status;
    if (stored.status == VulkanStoredPipelineCacheStatus::DifferentDriver ||
        stored.status == VulkanStoredPipelineCacheStatus::Damaged) {
      ALCEDO_LOG_INFO(Gpu) << "Vulkan: the pipeline cache file " << cache_file_.string()
                           << " is not used (" << VulkanStoredPipelineCacheStatusName(stored.status)
                           << ": " << stored.detail << "); the next store replaces it.";
    }
  }

  VkPipelineCacheCreateInfo create_info{};
  create_info.sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  create_info.initialDataSize = stored.data.size();
  create_info.pInitialData    = stored.data.empty() ? nullptr : stored.data.data();
  ThrowIfVulkanError(device_.functions->vkCreatePipelineCache(device_.device, &create_info, nullptr,
                                                              &pipeline_cache_),
                     "vkCreatePipelineCache");
}

VulkanComputePipelineCache::~VulkanComputePipelineCache() {
  for (auto& [_, entry] : entries_) {
    DestroyEntry(*entry);
  }
  device_.functions->vkDestroyPipelineCache(device_.device, pipeline_cache_, nullptr);
}

void VulkanComputePipelineCache::DestroyEntry(Entry& entry) noexcept {
  const auto& functions = *device_.functions;
  if (entry.pipeline.pipeline != VK_NULL_HANDLE) {
    functions.vkDestroyPipeline(device_.device, entry.pipeline.pipeline, nullptr);
  }
  if (entry.pipeline.layout != VK_NULL_HANDLE) {
    functions.vkDestroyPipelineLayout(device_.device, entry.pipeline.layout, nullptr);
  }
  if (entry.pipeline.set_layout != VK_NULL_HANDLE) {
    functions.vkDestroyDescriptorSetLayout(device_.device, entry.pipeline.set_layout, nullptr);
  }
  entry.pipeline = {};
}

auto VulkanComputePipelineCache::Pipeline(const VulkanKernelDescriptor& descriptor)
    -> const VulkanComputePipeline& {
  auto            bindings = CheckedBindings(descriptor, device_.max_push_constant_bytes);
  Key             key{descriptor.kernel, SortedSpecialization(descriptor)};

  std::lock_guard lock(mutex_);
  // The layout of a kernel is a property of its source: each specialization has the same one.
  const auto      layout = kernel_layouts_.find(key.kernel);
  if (layout != kernel_layouts_.end() &&
      (layout->second.bindings != bindings ||
       layout->second.push_constant_bytes != descriptor.push_constant_bytes)) {
    throw std::logic_error("Vulkan kernel " + key.kernel +
                           ": a request gives other bindings or another push constant size than "
                           "an earlier request.");
  }
  if (const auto found = entries_.find(key); found != entries_.end()) {
    NoteVulkanPipelineCacheHit();
    return found->second->pipeline;
  }

  const VkShaderModule module    = modules_->Module(key.kernel);
  const auto&          functions = *device_.functions;
  auto                 entry     = std::make_unique<Entry>();
  try {
    if (!bindings.empty()) {
      std::vector<VkDescriptorSetLayoutBinding> layout_bindings;
      for (const auto& binding : bindings) {
        VkDescriptorSetLayoutBinding layout_binding{};
        layout_binding.binding         = binding.binding;
        layout_binding.descriptorType  = binding.type;
        layout_binding.descriptorCount = 1;
        layout_binding.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        layout_bindings.push_back(layout_binding);
      }
      VkDescriptorSetLayoutCreateInfo set_info{};
      set_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      set_info.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
      set_info.bindingCount = static_cast<uint32_t>(layout_bindings.size());
      set_info.pBindings    = layout_bindings.data();
      ThrowIfVulkanError(functions.vkCreateDescriptorSetLayout(device_.device, &set_info, nullptr,
                                                               &entry->pipeline.set_layout),
                         "vkCreateDescriptorSetLayout (" + key.kernel + ")");
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset     = 0;
    push_range.size       = descriptor.push_constant_bytes;
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (entry->pipeline.set_layout != VK_NULL_HANDLE) {
      layout_info.setLayoutCount = 1;
      layout_info.pSetLayouts    = &entry->pipeline.set_layout;
    }
    if (descriptor.push_constant_bytes > 0) {
      layout_info.pushConstantRangeCount = 1;
      layout_info.pPushConstantRanges    = &push_range;
    }
    ThrowIfVulkanError(functions.vkCreatePipelineLayout(device_.device, &layout_info, nullptr,
                                                        &entry->pipeline.layout),
                       "vkCreatePipelineLayout (" + key.kernel + ")");

    std::vector<VkSpecializationMapEntry> map_entries;
    std::vector<uint32_t>                 values;
    for (const auto& value : key.specialization) {
      VkSpecializationMapEntry map_entry{};
      map_entry.constantID = value.constant_id;
      map_entry.offset     = static_cast<uint32_t>(values.size() * sizeof(uint32_t));
      map_entry.size       = sizeof(uint32_t);
      map_entries.push_back(map_entry);
      values.push_back(value.value);
    }
    VkSpecializationInfo specialization{};
    specialization.mapEntryCount = static_cast<uint32_t>(map_entries.size());
    specialization.pMapEntries   = map_entries.data();
    specialization.dataSize      = values.size() * sizeof(uint32_t);
    specialization.pData         = values.data();

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType                     = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.flags                     = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
    pipeline_info.stage.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage               = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module              = module;
    pipeline_info.stage.pName               = key.kernel.c_str();
    pipeline_info.stage.pSpecializationInfo = map_entries.empty() ? nullptr : &specialization;
    pipeline_info.layout                    = entry->pipeline.layout;
    ThrowIfVulkanError(
        functions.vkCreateComputePipelines(device_.device, pipeline_cache_, 1, &pipeline_info,
                                           nullptr, &entry->pipeline.pipeline),
        "vkCreateComputePipelines (" + key.kernel + ")");
  } catch (...) {
    DestroyEntry(*entry);
    throw;
  }
  NoteVulkanPipelineCreation();
  if (layout == kernel_layouts_.end()) {
    kernel_layouts_.emplace(key.kernel,
                            KernelLayout{std::move(bindings), descriptor.push_constant_bytes});
  }
  auto [inserted, _] = entries_.emplace(std::move(key), std::move(entry));
  return inserted->second->pipeline;
}

void VulkanComputePipelineCache::StoreToDisk() {
  if (cache_file_.empty()) {
    return;
  }
  std::lock_guard lock(mutex_);
  std::size_t     size = 0;
  ThrowIfVulkanError(
      device_.functions->vkGetPipelineCacheData(device_.device, pipeline_cache_, &size, nullptr),
      "vkGetPipelineCacheData");
  std::vector<std::byte> data(size);
  ThrowIfVulkanError(device_.functions->vkGetPipelineCacheData(device_.device, pipeline_cache_,
                                                               &size, data.data()),
                     "vkGetPipelineCacheData");
  data.resize(size);
  WriteStoredPipelineCacheData(cache_file_, data);
}

}  // namespace alcedo::vulkan
