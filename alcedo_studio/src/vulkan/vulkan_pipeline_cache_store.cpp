//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "vulkan/vulkan_pipeline_cache_store.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

#include "utils/hash/sha256.hpp"

namespace alcedo::vulkan {
namespace {

/// File header of the application, before the data of `vkGetPipelineCacheData`.
/// Little-endian: magic (8 bytes), format version (4), reserved (4), data size (8), SHA-256 (32).
constexpr std::array<char, 8> kFileMagic         = {'A', 'L', 'C', 'D', 'V', 'K', 'P', 'C'};
constexpr uint32_t            kFileFormatVersion = 1;
constexpr std::size_t         kFileHeaderBytes   = 8 + 4 + 4 + 8 + 32;
/// `VkPipelineCacheHeaderVersionOne`: header size, header version, vendor, device, UUID.
constexpr std::size_t         kVulkanHeaderBytes = 16 + VK_UUID_SIZE;

template <typename T>
auto ReadValue(std::span<const std::byte> bytes, std::size_t offset) -> T {
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

template <typename T>
void AppendValue(std::vector<std::byte>& bytes, const T& value) {
  const auto* begin = reinterpret_cast<const std::byte*>(&value);
  bytes.insert(bytes.end(), begin, begin + sizeof(T));
}

auto Damaged(std::string detail) -> VulkanStoredPipelineCacheData {
  VulkanStoredPipelineCacheData result;
  result.status = VulkanStoredPipelineCacheStatus::Damaged;
  result.detail = std::move(detail);
  return result;
}

auto HexOf(std::span<const uint8_t> bytes) -> std::string {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string           text;
  for (const uint8_t byte : bytes) {
    text.push_back(kDigits[byte >> 4U]);
    text.push_back(kDigits[byte & 0x0FU]);
  }
  return text;
}

}  // namespace

auto VulkanStoredPipelineCacheStatusName(VulkanStoredPipelineCacheStatus status) -> const char* {
  switch (status) {
    case VulkanStoredPipelineCacheStatus::NoFile:
      return "NoFile";
    case VulkanStoredPipelineCacheStatus::Loaded:
      return "Loaded";
    case VulkanStoredPipelineCacheStatus::DifferentDriver:
      return "DifferentDriver";
    case VulkanStoredPipelineCacheStatus::Damaged:
      return "Damaged";
  }
  return "Unknown";
}

auto ReadPipelineCacheDataIdentity(std::span<const std::byte> data)
    -> std::optional<VulkanPipelineCacheIdentity> {
  if (data.size() < kVulkanHeaderBytes) {
    return std::nullopt;
  }
  const auto header_size    = ReadValue<uint32_t>(data, 0);
  const auto header_version = ReadValue<uint32_t>(data, 4);
  if (header_size < kVulkanHeaderBytes ||
      header_version != static_cast<uint32_t>(VK_PIPELINE_CACHE_HEADER_VERSION_ONE)) {
    return std::nullopt;
  }
  VulkanPipelineCacheIdentity identity;
  identity.vendor_id = ReadValue<uint32_t>(data, 8);
  identity.device_id = ReadValue<uint32_t>(data, 12);
  std::memcpy(identity.pipeline_cache_uuid.data(), data.data() + 16, VK_UUID_SIZE);
  return identity;
}

auto ReadStoredPipelineCacheData(const std::filesystem::path&       path,
                                 const VulkanPipelineCacheIdentity& identity)
    -> VulkanStoredPipelineCacheData {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    return {};
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Damaged("the file cannot be read");
  }
  std::vector<char>      chars((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
  std::vector<std::byte> file(chars.size());
  std::memcpy(file.data(), chars.data(), chars.size());

  if (file.size() < kFileHeaderBytes ||
      std::memcmp(file.data(), kFileMagic.data(), kFileMagic.size()) != 0) {
    return Damaged("no pipeline cache file header");
  }
  if (ReadValue<uint32_t>(file, 8) != kFileFormatVersion) {
    return Damaged("file format version " + std::to_string(ReadValue<uint32_t>(file, 8)));
  }
  const auto data_size = ReadValue<uint64_t>(file, 16);
  if (data_size != file.size() - kFileHeaderBytes) {
    return Damaged("the data has " + std::to_string(file.size() - kFileHeaderBytes) +
                   " bytes; the header records " + std::to_string(data_size));
  }
  const std::span<const std::byte> data(file.data() + kFileHeaderBytes, data_size);
  const auto                       checksum = ComputeSha256(data);
  if (std::memcmp(checksum.data(), file.data() + 24, checksum.size()) != 0) {
    return Damaged("the SHA-256 of the data differs from the header");
  }

  const auto stored = ReadPipelineCacheDataIdentity(data);
  if (!stored.has_value()) {
    return Damaged("the data has no VkPipelineCacheHeaderVersionOne");
  }
  if (stored->vendor_id != identity.vendor_id || stored->device_id != identity.device_id ||
      stored->pipeline_cache_uuid != identity.pipeline_cache_uuid) {
    VulkanStoredPipelineCacheData result;
    result.status = VulkanStoredPipelineCacheStatus::DifferentDriver;
    result.detail =
        "stored vendor " + std::to_string(stored->vendor_id) + " device " +
        std::to_string(stored->device_id) + " cache UUID " + HexOf(stored->pipeline_cache_uuid) +
        "; this device has vendor " + std::to_string(identity.vendor_id) + " device " +
        std::to_string(identity.device_id) + " cache UUID " + HexOf(identity.pipeline_cache_uuid);
    return result;
  }

  VulkanStoredPipelineCacheData result;
  result.status = VulkanStoredPipelineCacheStatus::Loaded;
  result.data.assign(data.begin(), data.end());
  return result;
}

void WriteStoredPipelineCacheData(const std::filesystem::path& path,
                                  std::span<const std::byte>   data) {
  std::vector<std::byte> file;
  file.reserve(kFileHeaderBytes + data.size());
  for (const char c : kFileMagic) {
    file.push_back(static_cast<std::byte>(c));
  }
  AppendValue(file, kFileFormatVersion);
  AppendValue(file, uint32_t{0});
  AppendValue(file, static_cast<uint64_t>(data.size()));
  const auto checksum = ComputeSha256(data);
  for (const uint8_t byte : checksum) {
    file.push_back(static_cast<std::byte>(byte));
  }
  file.insert(file.end(), data.begin(), data.end());

  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      throw std::runtime_error("Vulkan: cannot create the pipeline cache folder " +
                               path.parent_path().string() + ": " + ec.message());
    }
  }
  auto temporary = path;
  temporary += ".tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(file.data()),
                 static_cast<std::streamsize>(file.size()));
    if (!stream) {
      throw std::runtime_error("Vulkan: cannot write the pipeline cache file " +
                               temporary.string());
    }
  }
  std::filesystem::rename(temporary, path, ec);
  if (ec) {
    std::filesystem::remove(temporary, ec);
    throw std::runtime_error("Vulkan: cannot replace the pipeline cache file " + path.string());
  }
}

}  // namespace alcedo::vulkan
