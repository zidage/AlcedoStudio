//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// VulkanShaderModuleRegistry and the kernel inventory: the folder rule (packaged folder before the
// build folder), the errors of an absent or a changed kernel, and one module for each group. The
// registry tests use the device of the local GPU and the test_fill module of the build.

#include "vulkan/vulkan_shader_module_registry.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "vulkan/vulkan_api_counters.hpp"
#include "vulkan/vulkan_context.hpp"
#include "vulkan/vulkan_kernel_inventory.hpp"
#include "vulkan/vulkan_test_fill_kernels.hpp"
#include "vulkan_test_device.hpp"

namespace alcedo::vulkan {
namespace {

using test_support::OptionsOfThisRun;
using test_support::TemporaryFolder;

auto BuildFolder() -> std::filesystem::path {
  return VulkanKernelFolderCandidatesOfThisProcess().build_folder;
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << text;
  ASSERT_TRUE(stream.good()) << path.string();
}

auto ReadText(const std::filesystem::path& path) -> std::string {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

/// The SHA-256 of the test_fill module of the build, from its inventory line.
auto BuildTestFillChecksum() -> std::string {
  const std::string line     = ReadText(BuildFolder() / "test_fill.kernels");
  const auto        last_tab = line.rfind('\t');
  return line.substr(last_tab + 1, 64);
}

/// Makes @p folder a packaged kernel folder with a copy of the test_fill module under
/// @p module_file and an inventory of @p kernel for it.
void MakePackagedFolder(const std::filesystem::path& folder, const std::string& module_file,
                        const std::string& kernel, const std::string& checksum) {
  std::filesystem::copy_file(BuildFolder() / "test_fill.spv", folder / module_file,
                             std::filesystem::copy_options::overwrite_existing);
  WriteText(folder / "packaged.kernels", kernel + "\t" + module_file + "\t" + checksum + "\n");
  WriteText(folder / std::string(kPackagedKernelAssetList), "");
}

auto Contains(const std::string& text, const std::string& part) -> bool {
  return text.find(part) != std::string::npos;
}

// --- Kernel inventory and folder rule (no GPU) -------------------------------------------------

TEST(VulkanKernelInventoryTest, BuildFolderHasTheInventoryOfTheTestFillGroup) {
  const auto inventory = VulkanKernelInventory::Read(
      ResolveVulkanKernelFolder(VulkanKernelFolderCandidatesOfThisProcess()));
  const auto names = inventory.KernelNames();
  EXPECT_NE(std::find(names.begin(), names.end(), "test_fill"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "test_fill_image"), names.end());
  const auto& entry = inventory.Find(test_fill_kernels::kFillBuffer);
  EXPECT_EQ(entry.module_file, "test_fill.spv");
  EXPECT_EQ(entry.inventory_file, "test_fill.kernels");
  EXPECT_EQ(entry.module_sha256.size(), 64U);
}

TEST(VulkanKernelInventoryTest, PackagedFolderWithAssetListIsSelectedBeforeTheBuildFolder) {
  TemporaryFolder packaged("folder_rule_packaged");
  TemporaryFolder build("folder_rule_build");
  WriteText(packaged.Path() / std::string(kPackagedKernelAssetList), "");

  const auto folder = ResolveVulkanKernelFolder({{packaged.Path()}, build.Path()});
  EXPECT_EQ(folder.folder, packaged.Path());
  EXPECT_EQ(folder.searched, std::vector<std::filesystem::path>{packaged.Path()});
}

TEST(VulkanKernelInventoryTest, PackagedFolderWithoutAssetListIsSkipped) {
  TemporaryFolder packaged("folder_rule_unpackaged");
  TemporaryFolder build("folder_rule_build_only");

  const auto      folder = ResolveVulkanKernelFolder({{packaged.Path()}, build.Path()});
  EXPECT_EQ(folder.folder, build.Path());
  EXPECT_EQ(folder.searched, (std::vector<std::filesystem::path>{packaged.Path(), build.Path()}));
}

TEST(VulkanKernelInventoryTest, MalformedLineReportsTheFileAndTheLineNumber) {
  TemporaryFolder folder("inventory_malformed");
  WriteText(folder.Path() / "group.kernels",
            "good_kernel\tgroup.spv\t" + std::string(64, 'a') + "\nbad kernel line\n");
  try {
    (void)VulkanKernelInventory::Read({folder.Path(), {folder.Path()}});
    FAIL() << "A malformed line was accepted.";
  } catch (const std::runtime_error& error) {
    EXPECT_TRUE(Contains(error.what(), "group.kernels:2")) << error.what();
  }
}

TEST(VulkanKernelInventoryTest, KernelNameInTwoInventoriesReportsBothFiles) {
  TemporaryFolder   folder("inventory_duplicate");
  const std::string checksum(64, 'b');
  WriteText(folder.Path() / "first.kernels", "shared_kernel\tfirst.spv\t" + checksum + "\n");
  WriteText(folder.Path() / "second.kernels", "shared_kernel\tsecond.spv\t" + checksum + "\n");
  try {
    (void)VulkanKernelInventory::Read({folder.Path(), {folder.Path()}});
    FAIL() << "A kernel name in two inventories was accepted.";
  } catch (const std::runtime_error& error) {
    EXPECT_TRUE(Contains(error.what(), "shared_kernel")) << error.what();
    EXPECT_TRUE(Contains(error.what(), "first.kernels")) << error.what();
    EXPECT_TRUE(Contains(error.what(), "second.kernels")) << error.what();
  }
}

TEST(VulkanKernelInventoryTest, SpirvEntryPointsOfTheTestFillModuleAreBothKernels) {
  const std::string     text = ReadText(BuildFolder() / "test_fill.spv");
  std::vector<uint32_t> words(text.size() / sizeof(uint32_t));
  std::memcpy(words.data(), text.data(), words.size() * sizeof(uint32_t));
  EXPECT_EQ(ReadSpirvEntryPointNames(words),
            (std::vector<std::string>{"test_fill", "test_fill_image"}));
  words[0] = 0;
  EXPECT_THROW((void)ReadSpirvEntryPointNames(words), std::runtime_error);
}

// --- Registry on the local GPU -----------------------------------------------------------------

class VulkanShaderModuleRegistryTest : public test_support::VulkanContextTestBase {
 protected:
  void SetUp() override {
    VulkanContextTestBase::SetUp();
    context_ = &VulkanContext::Create(OptionsOfThisRun());
  }

  /// A registry of the pipeline device with other kernel folders than the context.
  auto MakeRegistry(VulkanKernelFolderCandidates candidates) -> VulkanShaderModuleRegistry {
    return VulkanShaderModuleRegistry(context_->PipelineQueue().device,
                                      context_->PipelineDeviceFunctions(), std::move(candidates));
  }

  VulkanContext* context_ = nullptr;
};

TEST_F(VulkanShaderModuleRegistryTest, PackagedPathIsUsedBeforeTheBuildTreePath) {
  TemporaryFolder packaged("registry_packaged");
  // The packaged module has a file name that the build folder does not have, so a module from
  // the build folder cannot satisfy the request.
  MakePackagedFolder(packaged.Path(), "packaged_test_fill.spv", "test_fill",
                     BuildTestFillChecksum());

  auto       registry = MakeRegistry({{packaged.Path()}, BuildFolder()});
  const auto before   = ReadVulkanApiCounters();
  EXPECT_NE(registry.Module(test_fill_kernels::kFillBuffer), VK_NULL_HANDLE);
  EXPECT_EQ(VulkanApiCounterDelta(before, ReadVulkanApiCounters()).module_loads, 1U);
  EXPECT_EQ(registry.Folder().folder, packaged.Path());
  // The packaged folder is the only source: its inventory does not list test_fill_image.
  EXPECT_EQ(registry.KernelNames(), std::vector<std::string>{"test_fill"});
  EXPECT_THROW((void)registry.Module(test_fill_kernels::kFillImage), std::runtime_error);
}

TEST_F(VulkanShaderModuleRegistryTest, AbsentKernelReportsItsNameAndTheSearchedPaths) {
  TemporaryFolder unpackaged("registry_unpackaged");
  auto            registry = MakeRegistry({{unpackaged.Path()}, BuildFolder()});
  const auto      before   = ReadVulkanApiCounters();
  try {
    (void)registry.Module("absent_kernel");
    FAIL() << "An absent kernel gave a module.";
  } catch (const std::runtime_error& error) {
    EXPECT_TRUE(Contains(error.what(), "absent_kernel")) << error.what();
    EXPECT_TRUE(Contains(error.what(), unpackaged.Path().string())) << error.what();
    EXPECT_TRUE(Contains(error.what(), BuildFolder().string())) << error.what();
  }
  EXPECT_EQ(VulkanApiCounterDelta(before, ReadVulkanApiCounters()).module_loads, 0U);
}

TEST_F(VulkanShaderModuleRegistryTest, SecondKernelOfOneGroupLoadsNoSecondModule) {
  auto&      registry = context_->ShaderModules();
  const auto before   = ReadVulkanApiCounters();
  const auto first    = registry.Module(test_fill_kernels::kFillBuffer);
  const auto second   = registry.Module(test_fill_kernels::kFillImage);
  const auto again    = registry.Module(test_fill_kernels::kFillBuffer);
  EXPECT_NE(first, VK_NULL_HANDLE);
  EXPECT_EQ(first, second);
  EXPECT_EQ(first, again);
  EXPECT_EQ(VulkanApiCounterDelta(before, ReadVulkanApiCounters()).module_loads, 1U);
}

TEST_F(VulkanShaderModuleRegistryTest, ModuleThatDiffersFromItsInventoryChecksumIsRejected) {
  TemporaryFolder packaged("registry_changed_module");
  MakePackagedFolder(packaged.Path(), "test_fill.spv", "test_fill", std::string(64, '0'));

  auto       registry = MakeRegistry({{packaged.Path()}, BuildFolder()});
  const auto before   = ReadVulkanApiCounters();
  try {
    (void)registry.Module(test_fill_kernels::kFillBuffer);
    FAIL() << "A module with another checksum was loaded.";
  } catch (const std::runtime_error& error) {
    EXPECT_TRUE(Contains(error.what(), (packaged.Path() / "test_fill.spv").string()))
        << error.what();
    EXPECT_TRUE(Contains(error.what(), "differs from its kernel inventory")) << error.what();
  }
  EXPECT_EQ(VulkanApiCounterDelta(before, ReadVulkanApiCounters()).module_loads, 0U);
}

TEST_F(VulkanShaderModuleRegistryTest, InventoryKernelWithoutAnEntryPointReportsKernelAndModule) {
  TemporaryFolder packaged("registry_no_entry_point");
  MakePackagedFolder(packaged.Path(), "test_fill.spv", "kernel_not_in_module",
                     BuildTestFillChecksum());

  auto registry = MakeRegistry({{packaged.Path()}, BuildFolder()});
  try {
    (void)registry.Module("kernel_not_in_module");
    FAIL() << "A kernel without an entry point gave a module.";
  } catch (const std::runtime_error& error) {
    EXPECT_TRUE(Contains(error.what(), "has no entry point kernel_not_in_module")) << error.what();
    EXPECT_TRUE(Contains(error.what(), (packaged.Path() / "test_fill.spv").string()))
        << error.what();
  }
}

}  // namespace
}  // namespace alcedo::vulkan
