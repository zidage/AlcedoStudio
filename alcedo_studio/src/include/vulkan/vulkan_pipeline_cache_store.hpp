//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <volk.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace alcedo::vulkan {

/// The values of the `VkPipelineCacheHeaderVersionOne` header that the driver of the device writes.
/// Pipeline cache data with other values is from a different driver or device.
struct VulkanPipelineCacheIdentity {
  uint32_t                          vendor_id = 0;
  uint32_t                          device_id = 0;
  std::array<uint8_t, VK_UUID_SIZE> pipeline_cache_uuid{};
};

/// The result of reading a stored pipeline cache file.
enum class VulkanStoredPipelineCacheStatus {
  NoFile,           ///< No file at the path. The cache starts empty.
  Loaded,           ///< The data is from the driver of this device.
  DifferentDriver,  ///< The header names another vendor, device, or driver. Not used; replaced.
  Damaged,          ///< The file is not a complete pipeline cache file. Not used; replaced.
};

/// The name of @p status, for example `DifferentDriver`.
[[nodiscard]] auto VulkanStoredPipelineCacheStatusName(VulkanStoredPipelineCacheStatus status)
    -> const char*;

/// The data of a stored pipeline cache file. @ref data is empty unless @ref status is `Loaded`.
struct VulkanStoredPipelineCacheData {
  VulkanStoredPipelineCacheStatus status = VulkanStoredPipelineCacheStatus::NoFile;
  std::vector<std::byte>          data;
  /// The reason of `DifferentDriver` or `Damaged`, for the log.
  std::string                     detail;
};

/**
 * @brief Reads the pipeline cache data that `WriteStoredPipelineCacheData` wrote to @p path.
 *
 * The file has a header of the application (a magic value, the data size, and the SHA-256 of the
 * data) and the data of `vkGetPipelineCacheData`. The data is used only when the file is complete
 * and its `VkPipelineCacheHeaderVersionOne` has the values of @p identity: some drivers do not
 * check the data that they get. Reads the file system; no other side effect; does not throw for
 * the content of the file.
 */
[[nodiscard]] auto ReadStoredPipelineCacheData(const std::filesystem::path&       path,
                                               const VulkanPipelineCacheIdentity& identity)
    -> VulkanStoredPipelineCacheData;

/**
 * @brief Writes @p data (from `vkGetPipelineCacheData`) to @p path in the format of
 * `ReadStoredPipelineCacheData`. Creates the parent folder. Writes a temporary file in the same
 * folder and renames it, so a reader never sees a partial file.
 * @throws std::runtime_error With the path when a write or the rename fails.
 */
void               WriteStoredPipelineCacheData(const std::filesystem::path& path,
                                                std::span<const std::byte>   data);

/// The identity of pipeline cache data: the vendor, the device, and the cache UUID of the
/// `VkPipelineCacheHeaderVersionOne` at the start of @p data. Empty when @p data has no such
/// header.
[[nodiscard]] auto ReadPipelineCacheDataIdentity(std::span<const std::byte> data)
    -> std::optional<VulkanPipelineCacheIdentity>;

}  // namespace alcedo::vulkan
