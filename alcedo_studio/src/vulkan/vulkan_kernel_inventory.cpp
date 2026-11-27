//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "vulkan/vulkan_kernel_inventory.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace alcedo::vulkan {
namespace {

auto IsRegularFile(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec) && !ec;
}

auto ExecutableFolder() -> std::filesystem::path {
#if defined(_WIN32)
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    const DWORD copied =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied == 0) {
      return {};
    }
    if (copied < buffer.size()) {
      buffer.resize(copied);
      return std::filesystem::path(buffer).parent_path();
    }
    buffer.resize(buffer.size() * 2);
  }
#else
  return {};
#endif
}

auto SearchedFoldersText(const VulkanKernelFolder& folder) -> std::string {
  std::string text;
  for (const auto& searched : folder.searched) {
    if (!text.empty()) {
      text += "; ";
    }
    text += searched.string();
  }
  return text.empty() ? std::string("(none)") : text;
}

auto IsLowerHexSha256(std::string_view text) -> bool {
  return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

auto IsIdentifier(std::string_view text) -> bool {
  if (text.empty() || std::isdigit(static_cast<unsigned char>(text.front())) != 0) {
    return false;
  }
  return std::all_of(text.begin(), text.end(), [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
  });
}

auto SplitTabs(std::string_view line) -> std::vector<std::string_view> {
  std::vector<std::string_view> fields;
  std::size_t                   start = 0;
  while (true) {
    const std::size_t tab = line.find('\t', start);
    fields.push_back(line.substr(start, tab == std::string_view::npos ? line.npos : tab - start));
    if (tab == std::string_view::npos) {
      return fields;
    }
    start = tab + 1;
  }
}

}  // namespace

auto ResolveVulkanKernelFolder(const VulkanKernelFolderCandidates& candidates)
    -> VulkanKernelFolder {
  VulkanKernelFolder result;
  for (const auto& packaged : candidates.packaged_folders) {
    result.searched.push_back(packaged);
    if (IsRegularFile(packaged / kPackagedKernelAssetList)) {
      result.folder = packaged;
      return result;
    }
  }
  if (!candidates.build_folder.empty()) {
    result.searched.push_back(candidates.build_folder);
    result.folder = candidates.build_folder;
  }
  return result;
}

auto VulkanKernelFolderCandidatesOfThisProcess() -> VulkanKernelFolderCandidates {
  VulkanKernelFolderCandidates candidates;
  const auto                   executable_folder = ExecutableFolder();
  if (!executable_folder.empty()) {
    candidates.packaged_folders.push_back(executable_folder / "vulkan");
  }
#ifdef ALCEDO_VULKAN_KERNEL_BUILD_ROOT
  candidates.build_folder = std::filesystem::path(ALCEDO_VULKAN_KERNEL_BUILD_ROOT);
#endif
  return candidates;
}

auto VulkanKernelInventory::Read(VulkanKernelFolder folder) -> VulkanKernelInventory {
  VulkanKernelInventory inventory;
  inventory.folder_    = std::move(folder);
  const auto&     path = inventory.folder_.folder;

  std::error_code ec;
  if (path.empty() || !std::filesystem::is_directory(path, ec) || ec) {
    return inventory;
  }

  std::vector<std::filesystem::path> files;
  for (const auto& item : std::filesystem::directory_iterator(path, ec)) {
    if (item.is_regular_file() && item.path().extension() == kKernelInventorySuffix) {
      files.push_back(item.path());
    }
  }
  if (ec) {
    throw std::runtime_error("Vulkan: cannot list the kernel folder " + path.string() + ": " +
                             ec.message());
  }
  std::sort(files.begin(), files.end());

  for (const auto& file : files) {
    std::ifstream stream(file);
    if (!stream) {
      throw std::runtime_error("Vulkan: cannot read the kernel inventory " + file.string());
    }
    std::string line;
    int         line_number = 0;
    while (std::getline(stream, line)) {
      ++line_number;
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.empty()) {
        continue;
      }
      const auto fields = SplitTabs(line);
      if (fields.size() != 3 || !IsIdentifier(fields[0]) || fields[1].empty() ||
          fields[1].find_first_of("/\\") != std::string_view::npos ||
          !IsLowerHexSha256(fields[2])) {
        throw std::runtime_error("Vulkan: malformed kernel inventory line " + file.string() + ":" +
                                 std::to_string(line_number) +
                                 " (expected <kernel>\\t<module file>\\t<SHA-256>).");
      }
      VulkanKernelInventoryEntry entry;
      entry.kernel         = std::string(fields[0]);
      entry.module_file    = std::string(fields[1]);
      entry.module_sha256  = std::string(fields[2]);
      entry.inventory_file = file.filename().string();
      const auto existing  = inventory.entries_.find(entry.kernel);
      if (existing != inventory.entries_.end()) {
        throw std::runtime_error("Vulkan: the kernel name " + entry.kernel + " is in " +
                                 existing->second.inventory_file + " and in " +
                                 entry.inventory_file + " of " + path.string() +
                                 "; a kernel name must be unique.");
      }
      inventory.entries_.emplace(entry.kernel, std::move(entry));
    }
  }
  return inventory;
}

auto VulkanKernelInventory::Find(std::string_view kernel) const
    -> const VulkanKernelInventoryEntry& {
  const auto it = entries_.find(kernel);
  if (it == entries_.end()) {
    throw std::runtime_error(
        "Vulkan: the kernel " + std::string(kernel) +
        " is not in the kernel inventory. Kernel folder: " +
        (folder_.folder.empty() ? std::string("(none)") : folder_.folder.string()) +
        ". Searched folders: " + SearchedFoldersText(folder_) + ".");
  }
  return it->second;
}

auto VulkanKernelInventory::KernelNames() const -> std::vector<std::string> {
  std::vector<std::string> names;
  names.reserve(entries_.size());
  for (const auto& [name, _] : entries_) {
    names.push_back(name);
  }
  return names;
}

}  // namespace alcedo::vulkan
