//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Resolve rule of the Vulkan accelerator preference (#1036, Vulkan plan phase VK0).

#include "edit/pipeline/pipeline_accelerator.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#ifdef HAVE_VULKAN
#include "vulkan/vulkan_context.hpp"
#endif

namespace alcedo {
namespace {

TEST(PipelineAcceleratorTest, AutoOnWindowsDoesNotResolveToVulkan) {
#if defined(_WIN32)
  EXPECT_NE(ResolveAcceleratorBackend(AcceleratorBackendPreference::Auto), GpuBackendKind::Vulkan);
#ifdef HAVE_VULKAN
  // Auto does not create the Vulkan context either.
  EXPECT_FALSE(vulkan::VulkanContext::IsCreated());
#endif
#else
  GTEST_SKIP() << "Auto selects Vulkan nowhere; this test covers the Windows rule.";
#endif
}

TEST(PipelineAcceleratorTest, VulkanPreferenceThrowsWhenTheBuildHasNoVulkan) {
#ifdef HAVE_VULKAN
  GTEST_SKIP() << "This build has Vulkan (ALCEDO_ENABLE_VULKAN=ON).";
#else
  EXPECT_FALSE(IsCompiledGpuBackend(GpuBackendKind::Vulkan));
  try {
    (void)ResolveAcceleratorBackend(AcceleratorBackendPreference::Vulkan);
    FAIL() << "ResolveAcceleratorBackend(Vulkan) did not throw.";
  } catch (const std::runtime_error& error) {
    EXPECT_EQ(std::string(error.what()), "Vulkan accelerator backend is not compiled.");
  }
#endif
}

TEST(PipelineAcceleratorTest, VulkanPreferenceResolvesToVulkanAndCreatesTheContext) {
#ifndef HAVE_VULKAN
  GTEST_SKIP() << "This build has no Vulkan (ALCEDO_ENABLE_VULKAN=OFF).";
#else
  ASSERT_FALSE(vulkan::VulkanContext::IsCreated());

  EXPECT_EQ(ResolveAcceleratorBackend(AcceleratorBackendPreference::Vulkan),
            GpuBackendKind::Vulkan);
  EXPECT_TRUE(vulkan::VulkanContext::IsCreated());
  // A second resolve uses the context of the process.
  const vulkan::VulkanContext* context = &vulkan::VulkanContext::Instance();
  EXPECT_EQ(ResolveAcceleratorBackend(AcceleratorBackendPreference::Vulkan),
            GpuBackendKind::Vulkan);
  EXPECT_EQ(&vulkan::VulkanContext::Instance(), context);

  EXPECT_TRUE(IsCompiledGpuBackend(GpuBackendKind::Vulkan));
  // No renderer and no geometry operator accepts Vulkan before the phases VK3 and VK17.
  EXPECT_FALSE(IsImplementedMergedPipelineBackend(GpuBackendKind::Vulkan));
  EXPECT_FALSE(IsImplementedGeometryOperatorBackend(GpuBackendKind::Vulkan));
  vulkan::VulkanContext::Destroy();
#endif
}

TEST(PipelineAcceleratorTest, VulkanPreferenceNameIsLowerCaseVulkan) {
  EXPECT_EQ(AcceleratorBackendPreferenceToString(AcceleratorBackendPreference::Vulkan), "vulkan");
}

}  // namespace
}  // namespace alcedo
