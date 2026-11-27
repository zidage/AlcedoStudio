//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <volk.h>

#include <compare>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "vulkan/vulkan_pipeline_cache_store.hpp"
#include "vulkan/vulkan_shader_module_registry.hpp"

namespace alcedo::vulkan {

/// One image binding of descriptor set 0 of a kernel (plan section 4.2: a kernel has one set, the
/// set holds images only, and buffers are device addresses in the push constants).
struct VulkanKernelBinding {
  uint32_t         binding = 0;
  /// `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE`, `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE`, or
  /// `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER`.
  VkDescriptorType type    = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

  friend auto operator==(const VulkanKernelBinding&, const VulkanKernelBinding&) -> bool = default;
};

/// One 32-bit specialization constant (`[vk::constant_id(constant_id)]` in Slang).
struct VulkanSpecializationValue {
  uint32_t    constant_id                                   = 0;
  uint32_t    value                                         = 0;

  friend auto operator<=>(const VulkanSpecializationValue&,
                          const VulkanSpecializationValue&) = default;
};

/**
 * @brief The layout of a kernel and the specialization of one pipeline.
 *
 * The kernel group header defines the kernel name, the binding numbers, and the push constant
 * size (plan section 4.2: one header for each kernel group). The binding list and the push
 * constant size are properties of the kernel: every request for one kernel must give the same
 * values.
 */
struct VulkanKernelDescriptor {
  std::string                            kernel;
  std::vector<VulkanKernelBinding>       bindings;
  /// Size of the push constant block of the kernel; a multiple of 4, at most
  /// `maxPushConstantsSize`.
  uint32_t                               push_constant_bytes = 0;
  std::vector<VulkanSpecializationValue> specialization;
};

/// A compute pipeline and its layout. The handles belong to `VulkanComputePipelineCache`.
struct VulkanComputePipeline {
  VkPipeline            pipeline   = VK_NULL_HANDLE;
  VkPipelineLayout      layout     = VK_NULL_HANDLE;
  /// The push descriptor set layout of set 0; `VK_NULL_HANDLE` for a kernel without bindings.
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
};

/// The device values that the pipeline cache needs.
struct VulkanComputePipelineCacheDevice {
  VkDevice                    device    = VK_NULL_HANDLE;
  const VolkDeviceTable*      functions = nullptr;
  VulkanPipelineCacheIdentity identity;
  uint32_t                    max_push_constant_bytes = 0;
};

/**
 * @brief Gives one compute pipeline for each kernel and specialization (plan section 4.1).
 *
 * The first request of a kernel and specialization gets the shader module from the registry and
 * creates the descriptor set layout (`VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR`),
 * the pipeline layout, and the pipeline (`VK_PIPELINE_CREATE_DISPATCH_BASE_BIT`, for the row bands
 * of plan section 4.2). Later requests return the same pipeline and create nothing. A pass encoder
 * gets a pipeline and does not create one.
 *
 * The `VkPipelineCache` of the device starts with the data of the cache file when the file is from
 * the driver of the device (`ReadStoredPipelineCacheData`); data from a different driver or a
 * damaged file is ignored, and `StoreToDisk` replaces it.
 *
 * Owner: `VulkanContext`. The registry must live longer than the cache, and the device must exist
 * until the destructor ran.
 *
 * Thread safety: `Pipeline` and `StoreToDisk` lock one mutex. The returned reference stays valid
 * for the lifetime of the cache.
 */
class VulkanComputePipelineCache {
 public:
  /// Reads the cache file (when @p cache_file is not empty) and creates the `VkPipelineCache`.
  /// @throws VulkanError When `vkCreatePipelineCache` fails.
  VulkanComputePipelineCache(VulkanComputePipelineCacheDevice device,
                             VulkanShaderModuleRegistry& modules, std::filesystem::path cache_file);
  ~VulkanComputePipelineCache();
  VulkanComputePipelineCache(const VulkanComputePipelineCache&)                    = delete;
  auto operator=(const VulkanComputePipelineCache&) -> VulkanComputePipelineCache& = delete;

  /**
   * @brief The pipeline of @p descriptor. Notes a cache hit or a pipeline creation.
   * @throws std::invalid_argument For a binding of a buffer type, a binding number used two times,
   *         a constant id used two times, or a push constant size that is not a multiple of 4 or
   *         is above the device limit.
   * @throws std::logic_error When an earlier request for the same kernel gave other bindings or
   *         another push constant size.
   * @throws std::runtime_error Or `VulkanError` from the registry and from the Vulkan calls, with
   *         the kernel name. A failed request leaves no entry and no Vulkan object.
   */
  [[nodiscard]] auto Pipeline(const VulkanKernelDescriptor& descriptor)
      -> const VulkanComputePipeline&;

  /// Writes the `VkPipelineCache` data to the cache file. No effect without a cache file.
  /// @throws VulkanError Or std::runtime_error With the path when a step fails.
  void               StoreToDisk();

  /// What the constructor found in the cache file.
  [[nodiscard]] auto StoredDataStatus() const -> VulkanStoredPipelineCacheStatus {
    return stored_data_status_;
  }
  [[nodiscard]] auto CacheFile() const -> const std::filesystem::path& { return cache_file_; }

 private:
  struct Key {
    std::string                            kernel;
    std::vector<VulkanSpecializationValue> specialization;  // sorted by constant id
    friend auto                            operator<=>(const Key&, const Key&) = default;
  };
  /// The layout of a kernel: the same for each of its specializations.
  struct KernelLayout {
    std::vector<VulkanKernelBinding> bindings;  // sorted by binding number
    uint32_t                         push_constant_bytes = 0;
  };
  struct Entry {
    VulkanComputePipeline pipeline;
  };

  void                             DestroyEntry(Entry& entry) noexcept;

  VulkanComputePipelineCacheDevice device_;
  VulkanShaderModuleRegistry*      modules_;
  std::filesystem::path            cache_file_;
  VulkanStoredPipelineCacheStatus  stored_data_status_ = VulkanStoredPipelineCacheStatus::NoFile;
  VkPipelineCache                  pipeline_cache_     = VK_NULL_HANDLE;
  std::mutex                       mutex_;
  std::map<Key, std::unique_ptr<Entry>>            entries_;
  /// The layout of each kernel that has a pipeline.
  std::map<std::string, KernelLayout, std::less<>> kernel_layouts_;
};

}  // namespace alcedo::vulkan
