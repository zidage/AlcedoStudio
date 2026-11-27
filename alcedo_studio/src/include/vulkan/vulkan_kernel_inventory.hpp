//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace alcedo::vulkan {

/// The file that the install writes into a packaged kernel folder (the GPU asset checksum list
/// that `alcedo-gpu-check` reads). Its presence makes the folder a packaged kernel folder.
inline constexpr std::string_view kPackagedKernelAssetList = "runtime-assets.sha256";

/// The file suffix of the kernel inventory of one kernel group (`<group>.kernels`).
inline constexpr std::string_view kKernelInventorySuffix   = ".kernels";

/// The folders that can hold the SPIR-V modules and the kernel inventories, in search order.
struct VulkanKernelFolderCandidates {
  /// Packaged kernel folders, for example `<executable folder>/vulkan`.
  std::vector<std::filesystem::path> packaged_folders;
  /// The kernel folder of the build tree (`ALCEDO_VULKAN_KERNEL_BUILD_ROOT`). Can be empty.
  std::filesystem::path              build_folder;
};

/// The kernel folder that a process uses, and the folders that the resolution examined.
struct VulkanKernelFolder {
  std::filesystem::path              folder;
  /// Each candidate that `ResolveVulkanKernelFolder` examined, in order, with the selected one.
  std::vector<std::filesystem::path> searched;
};

/**
 * @brief Selects the kernel folder of the process.
 *
 * The first packaged folder that has `runtime-assets.sha256` is selected, as
 * `ResolveOpenClSourcePath` does for OpenCL: a packaged folder is then the only source, also when a
 * kernel is absent from it, so a package never reads the kernels of the build computer. Without
 * such a folder the build folder is selected. Reads the file system; no other side effect.
 */
[[nodiscard]] auto ResolveVulkanKernelFolder(const VulkanKernelFolderCandidates& candidates)
    -> VulkanKernelFolder;

/// The candidates of this process: `<executable folder>/vulkan`, then the build folder of the
/// build that made the executable.
[[nodiscard]] auto VulkanKernelFolderCandidatesOfThisProcess() -> VulkanKernelFolderCandidates;

/// One line of a kernel inventory.
struct VulkanKernelInventoryEntry {
  std::string kernel;
  /// The file name of the SPIR-V module of the kernel group, in the kernel folder.
  std::string module_file;
  /// Lower-case hexadecimal SHA-256 of the module file.
  std::string module_sha256;
  /// The inventory file that has the line.
  std::string inventory_file;
};

/**
 * @brief The kernels of one kernel folder: the union of its `<group>.kernels` files.
 *
 * The build rule `alcedo_add_slang_kernels` writes one inventory for each kernel group. Each line
 * is `<kernel>\t<module file>\t<SHA-256>`. The value is immutable after `Read`.
 */
class VulkanKernelInventory {
 public:
  /**
   * @brief Reads every inventory file of @p folder. A folder that does not exist gives an empty
   * inventory, so a missing kernel is reported by `Find` with the searched folders.
   * @throws std::runtime_error With the file and the line number for a malformed line, and with
   *         both files for a kernel name that two lines have (a kernel name is unique in the
   *         process).
   */
  [[nodiscard]] static auto Read(VulkanKernelFolder folder) -> VulkanKernelInventory;

  /// The entry of @p kernel. @throws std::runtime_error With the kernel name, the kernel folder,
  /// and the searched folders when the inventory has no entry.
  [[nodiscard]] auto Find(std::string_view kernel) const -> const VulkanKernelInventoryEntry&;

  /// The kernel names, sorted.
  [[nodiscard]] auto KernelNames() const -> std::vector<std::string>;

  [[nodiscard]] auto Folder() const -> const VulkanKernelFolder& { return folder_; }

 private:
  VulkanKernelFolder                                             folder_;
  std::map<std::string, VulkanKernelInventoryEntry, std::less<>> entries_;
};

}  // namespace alcedo::vulkan
