//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <volk.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "vulkan/vulkan_kernel_inventory.hpp"

namespace alcedo::vulkan {

/// The names of the `OpEntryPoint` instructions of a SPIR-V module, in module order.
/// @throws std::runtime_error When @p words is not a SPIR-V module (magic number, word count).
[[nodiscard]] auto ReadSpirvEntryPointNames(std::span<const uint32_t> words)
    -> std::vector<std::string>;

/**
 * @brief Gives the `VkShaderModule` of a kernel by its name (plan section 4.1).
 *
 * The registry reads the kernel inventory of the kernel folder at the first request
 * (`ResolveVulkanKernelFolder`: the packaged folder before the build folder). For a kernel it
 * reads the module file of its group, checks the SHA-256 of the file against the inventory and the
 * entry point of the kernel in the module, and creates the shader module one time for each group.
 * The entry point name of the kernel is the kernel name. The application compiles no shader
 * source at run time.
 *
 * Owner: `VulkanContext`, one registry for the pipeline device. The registry destroys its modules
 * in its destructor; the device must exist until then.
 *
 * Thread safety: `Module`, `Folder`, and `KernelNames` lock one mutex.
 */
class VulkanShaderModuleRegistry {
 public:
  /// @p functions must stay valid for the lifetime of the registry. No Vulkan call.
  VulkanShaderModuleRegistry(VkDevice device, const VolkDeviceTable& functions,
                             VulkanKernelFolderCandidates candidates);
  ~VulkanShaderModuleRegistry();
  VulkanShaderModuleRegistry(const VulkanShaderModuleRegistry&)                    = delete;
  auto operator=(const VulkanShaderModuleRegistry&) -> VulkanShaderModuleRegistry& = delete;

  /**
   * @brief The shader module of the group that has @p kernel. The first request of a group
   * creates the module (`NoteVulkanModuleLoad`); later requests return the same handle.
   * @throws std::runtime_error With the kernel name and the searched folders when no inventory has
   *         the kernel; with the module path when the file is absent, differs from its inventory
   *         checksum, is not SPIR-V, or has no entry point of that name; or a `VulkanError` from
   *         `vkCreateShaderModule`. A failed request creates nothing; a later request runs again.
   */
  [[nodiscard]] auto Module(std::string_view kernel) -> VkShaderModule;

  /// The kernel folder of the registry. Reads the inventory at the first call.
  [[nodiscard]] auto Folder() -> VulkanKernelFolder;

  /// The kernel names of the inventory, sorted. Reads the inventory at the first call.
  [[nodiscard]] auto KernelNames() -> std::vector<std::string>;

 private:
  /// The inventory; read at the first use. The caller holds @ref mutex_.
  auto InventoryLocked() -> const VulkanKernelInventory&;

  struct LoadedModule {
    VkShaderModule           module = VK_NULL_HANDLE;
    std::vector<std::string> entry_points;
  };

  VkDevice                                         device_;
  const VolkDeviceTable*                           functions_;
  VulkanKernelFolderCandidates                     candidates_;
  std::mutex                                       mutex_;
  std::optional<VulkanKernelInventory>             inventory_;
  /// Loaded modules by module file name.
  std::map<std::string, LoadedModule, std::less<>> modules_;
};

}  // namespace alcedo::vulkan
