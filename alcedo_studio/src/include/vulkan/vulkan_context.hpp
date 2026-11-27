//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// volk.h first: it defines VK_NO_PROTOTYPES for the Vulkan headers that VMA includes.
// clang-format off
#include <volk.h>
#include <vma/vk_mem_alloc.h>
// clang-format on

#include "vulkan/vulkan_compute_pipeline_cache.hpp"
#include "vulkan/vulkan_device_capabilities.hpp"
#include "vulkan/vulkan_kernel_inventory.hpp"
#include "vulkan/vulkan_shader_module_registry.hpp"

namespace alcedo::vulkan {

/// The options of `VulkanContext::Create`.
struct VulkanInitializationOptions {
  /// Part of a device name, case-insensitive. Empty: the default rule of `SelectVulkanDevice`.
  std::string               device_name_filter;
  /// Enables `VK_LAYER_KHRONOS_validation` when the layer is installed. A validation error is
  /// written to the `alcedo.gpu` log and counted (`VulkanContext::ValidationErrorCount`).
  bool                      enable_validation_layer = false;
  /// The folders of the kernel modules. Empty: `VulkanKernelFolderCandidatesOfThisProcess()`.
  std::optional<VulkanKernelFolderCandidates> kernel_folders;
  /// The file of the `VkPipelineCache` data (plan section 4.1: in the application cache folder).
  /// Empty: the pipeline cache is not read from or written to a file.
  std::filesystem::path                       pipeline_cache_file;

  /// The options of the application: `device_name_filter` from the environment variable
  /// `ALCEDO_VULKAN_DEVICE`, and the validation layer in a debug build (`NDEBUG` not defined).
  /// `pipeline_cache_file` stays empty: the application gives its cache folder.
  [[nodiscard]] static auto FromEnvironment() -> VulkanInitializationOptions;
};

/// The queue of the pipeline device. The render devices submit to @ref queue only through
/// `VulkanContext::SubmitToPipelineQueue`, which holds the queue lock (plan section 4.2).
struct VulkanPipelineQueue {
  VkDevice device       = VK_NULL_HANDLE;
  VkQueue  queue        = VK_NULL_HANDLE;
  uint32_t family_index = 0;
  uint32_t queue_index  = 0;
};

/**
 * @brief The process owner of the Vulkan instance, the physical device, the pipeline device with
 * its queue, the VMA allocator, the shader module registry, and the compute pipeline cache of the
 * pipeline device (plan section 4.1).
 *
 * There is one context in each process. `Create` makes it, `Instance` reads it, and `Destroy`
 * releases it (application shutdown, the device restore procedure of plan section 9.5, and tests
 * that select another device). The display device of plan decision B5 comes with phase VK18.
 *
 * Device-level Vulkan functions go through `PipelineDeviceFunctions()`, not through global
 * function pointers: on Windows the context can be a DLL (as `OpenClContext` is) and the volk
 * globals are not exported from it.
 *
 * Thread safety: `Create`, `Instance`, `IsCreated`, and `Destroy` lock one mutex. The caller must
 * not call `Destroy` while another thread uses the context.
 */
class VulkanContext {
 public:
  /**
   * @brief Creates the context of the process.
   *
   * Loads the Vulkan loader, creates the instance (Vulkan 1.3), reads the capabilities of each
   * physical device, selects one with `SelectVulkanDevice`, creates the pipeline device with one
   * queue of the pipeline queue family (`FindPipelineQueueFamily`), and creates the allocator.
   *
   * @throws std::logic_error When a context exists.
   * @throws std::runtime_error With the device name and the absent requirement when no device is
   *         usable, or a `VulkanError` when a Vulkan call fails. On failure no context exists and
   *         every object that the call created is destroyed; a later `Create` can run again.
   */
  static auto Create(const VulkanInitializationOptions& options) -> VulkanContext&;

  /// The context of the process; creates it with @p options as `Create` does when it does not
  /// exist. One lock covers the check and the creation. @p options has no effect on an existing
  /// context. @throws The exceptions of `Create`.
  static auto InstanceOrCreate(const VulkanInitializationOptions& options) -> VulkanContext&;

  /// The context of the process. @throws std::logic_error When `Create` has not run.
  static auto Instance() -> VulkanContext&;

  /// True when the context of the process exists.
  static auto IsCreated() -> bool;

  /// Waits until the pipeline device is idle and destroys the context. No effect without one.
  /// A context that exists at process exit is not destroyed by a static destructor (that would
  /// run under the loader lock of the DLL unload); the driver releases it with the process.
  static void Destroy();

  /// Validation errors that the validation layer reported in this process, over all contexts,
  /// including the creation and the destruction of the instance. Loader messages (message type
  /// GENERAL) go to the log as warnings and are not counted.
  static auto ValidationErrorCount() -> uint64_t;

  ~VulkanContext();
  VulkanContext(const VulkanContext&)                             = delete;
  auto operator=(const VulkanContext&) -> VulkanContext&          = delete;
  VulkanContext(VulkanContext&&)                                  = delete;
  auto               operator=(VulkanContext&&) -> VulkanContext& = delete;

  [[nodiscard]] auto VulkanInstance() const -> VkInstance { return instance_; }
  [[nodiscard]] auto PhysicalDevice() const -> VkPhysicalDevice { return physical_device_; }
  /// The capabilities of the selected physical device.
  [[nodiscard]] auto Capabilities() const -> const VulkanDeviceCapabilities& {
    return capabilities_;
  }
  [[nodiscard]] auto PipelineQueue() const -> const VulkanPipelineQueue& { return pipeline_queue_; }
  /**
   * @brief Submits @p submit to the pipeline queue with @p fence, under the queue lock.
   *
   * Several render devices (editor, thumbnails, export) run on threads of their own and share the
   * pipeline queue. A `VkQueue` needs external synchronization, so each submission to it goes
   * through this function. The lock covers `vkQueueSubmit2` only; the caller waits for @p fence
   * without the lock.
   *
   * @throws VulkanError When `vkQueueSubmit2` fails.
   */
  void               SubmitToPipelineQueue(const VkSubmitInfo2& submit, VkFence fence);
  /// The device-level functions of the pipeline device.
  [[nodiscard]] auto PipelineDeviceFunctions() const -> const VolkDeviceTable& {
    return pipeline_device_functions_;
  }
  /// The VMA allocator of the pipeline device (`VK_EXT_memory_budget`, buffer device addresses,
  /// and `VK_EXT_memory_priority` when the device has it).
  [[nodiscard]] auto PipelineAllocator() const -> VmaAllocator { return pipeline_allocator_; }
  /// The extensions that the pipeline device has enabled, sorted.
  [[nodiscard]] auto PipelineDeviceExtensions() const -> const std::vector<std::string>& {
    return pipeline_device_extensions_;
  }
  /// True when the instance has `VK_LAYER_KHRONOS_validation`.
  [[nodiscard]] auto ValidationLayerEnabled() const -> bool {
    return debug_messenger_ != VK_NULL_HANDLE;
  }
  /// The shader modules of the pipeline device (kernel folder of the options).
  [[nodiscard]] auto ShaderModules() -> VulkanShaderModuleRegistry& { return *shader_modules_; }
  /// The compute pipelines of the pipeline device. `Destroy` writes its data to the
  /// `pipeline_cache_file` of the options.
  [[nodiscard]] auto ComputePipelines() -> VulkanComputePipelineCache& {
    return *compute_pipelines_;
  }

 private:
  VulkanContext() = default;

  /// Creates the objects of a new context. The caller holds the lock of the process context.
  static auto CreateUnlocked(const VulkanInitializationOptions& options)
      -> std::unique_ptr<VulkanContext>;

  void                     CreateInstance(const VulkanInitializationOptions& options);
  void                     SelectPhysicalDevice(const VulkanInitializationOptions& options);
  void                     CreatePipelineDevice();
  void                     CreatePipelineAllocator();
  void                     CreateKernelObjects(const VulkanInitializationOptions& options);
  /// Destroys the objects in the reverse order of creation. Safe on a partly created context.
  void                     Release() noexcept;

  VkInstance               instance_        = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
  VkPhysicalDevice         physical_device_ = VK_NULL_HANDLE;
  VulkanDeviceCapabilities capabilities_;
  VulkanPipelineQueue      pipeline_queue_;
  std::mutex               pipeline_queue_mutex_;
  VolkDeviceTable          pipeline_device_functions_{};
  std::vector<std::string> pipeline_device_extensions_;
  VmaAllocator             pipeline_allocator_ = VK_NULL_HANDLE;
  std::unique_ptr<VulkanShaderModuleRegistry> shader_modules_;
  std::unique_ptr<VulkanComputePipelineCache> compute_pipelines_;
  bool                     loader_initialized_ = false;
};

}  // namespace alcedo::vulkan
