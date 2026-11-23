//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "vulkan/vulkan_shader_module_registry.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "utils/hash/sha256.hpp"
#include "vulkan/vulkan_api_counters.hpp"
#include "vulkan/vulkan_result.hpp"

namespace alcedo::vulkan {
namespace {

constexpr uint32_t kSpirvMagic         = 0x07230203U;
constexpr uint32_t kSpirvHeaderWords   = 5;
constexpr uint32_t kOpEntryPoint       = 15;
/// OpEntryPoint: opcode word, execution model, entry point id, then the name literal.
constexpr uint32_t kEntryPointNameWord = 3;

auto               ReadFileBytes(const std::filesystem::path& path) -> std::vector<std::byte> {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("Vulkan: cannot read the kernel module " + path.string());
  }
  std::vector<char>      chars((std::istreambuf_iterator<char>(stream)),
                                             std::istreambuf_iterator<char>());
  std::vector<std::byte> bytes(chars.size());
  std::memcpy(bytes.data(), chars.data(), chars.size());
  return bytes;
}

/// A SPIR-V literal string: UTF-8 bytes in words, ending with a zero byte.
auto ReadLiteralString(std::span<const uint32_t> words) -> std::string {
  std::string text;
  for (const uint32_t word : words) {
    for (int byte = 0; byte < 4; ++byte) {
      const char c = static_cast<char>((word >> (8 * byte)) & 0xFFU);
      if (c == '\0') {
        return text;
      }
      text.push_back(c);
    }
  }
  throw std::runtime_error("Vulkan: a SPIR-V entry point name has no end.");
}

}  // namespace

auto ReadSpirvEntryPointNames(std::span<const uint32_t> words) -> std::vector<std::string> {
  if (words.size() < kSpirvHeaderWords || words[0] != kSpirvMagic) {
    throw std::runtime_error("Vulkan: the file is not a SPIR-V module (no SPIR-V magic number).");
  }
  std::vector<std::string> names;
  std::size_t              index = kSpirvHeaderWords;
  while (index < words.size()) {
    const uint32_t word_count = words[index] >> 16U;
    const uint32_t opcode     = words[index] & 0xFFFFU;
    if (word_count == 0 || index + word_count > words.size()) {
      throw std::runtime_error(
          "Vulkan: the SPIR-V module has an instruction with a wrong word "
          "count at word " +
          std::to_string(index) + ".");
    }
    if (opcode == kOpEntryPoint && word_count > kEntryPointNameWord) {
      names.push_back(ReadLiteralString(
          words.subspan(index + kEntryPointNameWord, word_count - kEntryPointNameWord)));
    }
    index += word_count;
  }
  return names;
}

VulkanShaderModuleRegistry::VulkanShaderModuleRegistry(VkDevice                     device,
                                                       const VolkDeviceTable&       functions,
                                                       VulkanKernelFolderCandidates candidates)
    : device_(device), functions_(&functions), candidates_(std::move(candidates)) {}

VulkanShaderModuleRegistry::~VulkanShaderModuleRegistry() {
  for (auto& [_, loaded] : modules_) {
    functions_->vkDestroyShaderModule(device_, loaded.module, nullptr);
  }
}

auto VulkanShaderModuleRegistry::InventoryLocked() -> const VulkanKernelInventory& {
  if (!inventory_.has_value()) {
    inventory_ = VulkanKernelInventory::Read(ResolveVulkanKernelFolder(candidates_));
  }
  return *inventory_;
}

auto VulkanShaderModuleRegistry::Module(std::string_view kernel) -> VkShaderModule {
  std::lock_guard lock(mutex_);
  const auto&     entry       = InventoryLocked().Find(kernel);
  const auto      module_path = inventory_->Folder().folder / entry.module_file;

  auto            loaded      = modules_.find(entry.module_file);
  if (loaded == modules_.end()) {
    const std::vector<std::byte> bytes    = ReadFileBytes(module_path);
    const std::string            checksum = ComputeSha256Hex(bytes);
    if (checksum != entry.module_sha256) {
      throw std::runtime_error("Vulkan: the kernel module " + module_path.string() +
                               " differs from its kernel inventory " + entry.inventory_file +
                               " (SHA-256 " + checksum + ", inventory " + entry.module_sha256 +
                               ").");
    }
    if (bytes.size() % sizeof(uint32_t) != 0) {
      throw std::runtime_error("Vulkan: the kernel module " + module_path.string() +
                               " is not SPIR-V (its size is not a multiple of 4 bytes).");
    }
    std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
    std::memcpy(words.data(), bytes.data(), bytes.size());

    LoadedModule module;
    try {
      module.entry_points = ReadSpirvEntryPointNames(words);
    } catch (const std::runtime_error& error) {
      throw std::runtime_error(std::string(error.what()) + " Module: " + module_path.string());
    }

    VkShaderModuleCreateInfo create_info{};
    create_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = bytes.size();
    create_info.pCode    = words.data();
    ThrowIfVulkanError(
        functions_->vkCreateShaderModule(device_, &create_info, nullptr, &module.module),
        "vkCreateShaderModule (" + module_path.string() + ")");
    NoteVulkanModuleLoad();
    loaded = modules_.emplace(entry.module_file, std::move(module)).first;
  }

  const auto& entry_points = loaded->second.entry_points;
  if (std::find(entry_points.begin(), entry_points.end(), kernel) == entry_points.end()) {
    throw std::runtime_error("Vulkan: the kernel module " + module_path.string() +
                             " has no entry point " + std::string(kernel) + ", but its inventory " +
                             entry.inventory_file + " lists it.");
  }
  return loaded->second.module;
}

auto VulkanShaderModuleRegistry::Folder() -> VulkanKernelFolder {
  std::lock_guard lock(mutex_);
  return InventoryLocked().Folder();
}

auto VulkanShaderModuleRegistry::KernelNames() -> std::vector<std::string> {
  std::lock_guard lock(mutex_);
  return InventoryLocked().KernelNames();
}

}  // namespace alcedo::vulkan
