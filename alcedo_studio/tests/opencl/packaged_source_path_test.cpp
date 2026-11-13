// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "opencl/opencl_program_library.hpp"

namespace alcedo {
namespace {

class PackagedOpenClPathTest : public ::testing::Test {
 protected:
  std::filesystem::path root;
  bool                  owns_root = false;
  std::filesystem::path relative  = "edit/runtime/opencl/shader/primary_grade.cl";
  std::filesystem::path source    = std::filesystem::path(ALCEDO_TEST_SOURCE_ROOT) / relative;

  void                  SetUp() override {
    std::wstring executable(32768, L'\0');
    const auto   length =
        GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    ASSERT_GT(length, 0U);
    ASSERT_LT(length, executable.size());
    executable.resize(length);
    root = std::filesystem::path(executable).parent_path() / "opencl";
    ASSERT_FALSE(std::filesystem::exists(root));
    owns_root = true;
    ASSERT_TRUE(std::filesystem::is_regular_file(source));
  }

  /// The install inventory, which only an installed shader root has.
  void WriteInventory() const {
    std::filesystem::create_directories(root);
    std::ofstream(root / "runtime-assets.sha256") << "\n";
  }

  void TearDown() override {
    if (owns_root) {
      std::filesystem::remove_all(root);
    }
  }
};

TEST_F(PackagedOpenClPathTest, BuildTreeWithoutPackagedRootUsesConfiguredSource) {
  EXPECT_EQ(detail::ResolveOpenClSourcePath(source), source);
}

TEST_F(PackagedOpenClPathTest, BuildTreeOpenClDirectoryWithoutInventoryUsesConfiguredSource) {
  // The build tree has the CMake binary directory of the OpenCL libraries beside alcedo_main.
  std::filesystem::create_directories(root / "CMakeFiles");
  std::filesystem::create_directories((root / relative).parent_path());
  EXPECT_EQ(detail::ResolveOpenClSourcePath(source), source);
}

TEST_F(PackagedOpenClPathTest, ExistingPackagedFileTakesPrecedenceOverCheckout) {
  WriteInventory();
  std::filesystem::create_directories((root / relative).parent_path());
  std::ofstream(root / relative) << "// packaged copy\n";
  EXPECT_EQ(detail::ResolveOpenClSourcePath(source), root / relative);
}

TEST_F(PackagedOpenClPathTest, MissingPackagedFileDoesNotReadCheckout) {
  WriteInventory();
  std::filesystem::create_directories((root / relative).parent_path());
  EXPECT_EQ(detail::ResolveOpenClSourcePath(source), root / relative);
}

TEST_F(PackagedOpenClPathTest, MissingPackagedSubdirectoryDoesNotReadCheckout) {
  WriteInventory();
  EXPECT_EQ(detail::ResolveOpenClSourcePath(source), root / relative);
}

}  // namespace
}  // namespace alcedo
