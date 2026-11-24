//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// RecordKernelDispatch on the local GPU (plan phase VK3) with the check kernel test_fill_image of
// VK1: it writes one value to each texel of an RGBA32F storage image at binding 0. Each test runs
// with the validation layer. Set ALCEDO_VULKAN_DEVICE (for example "Iris") to select the device.

#include "edit/runtime/vulkan/vulkan_kernel_dispatch.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "edit/runtime/texture_format.hpp"
#include "vulkan/vulkan_api_counters.hpp"
#include "vulkan/vulkan_test_fill_kernels.hpp"
#include "vulkan_workspace_test_support.hpp"

namespace alcedo {
namespace {

using vulkan_workspace_test::VulkanWorkspaceFixture;
namespace fill                  = vulkan::test_fill_kernels;

constexpr std::uint32_t kWidth  = 37;  // not a multiple of the 8 x 8 workgroup
constexpr std::uint32_t kHeight = 21;

auto                    FillPush(std::array<float, 4> value) -> fill::FillImagePushConstants {
  fill::FillImagePushConstants push;
  for (std::size_t c = 0; c < 4; ++c) {
    push.value[c] = value[c];
  }
  push.width  = kWidth;
  push.height = kHeight;
  return push;
}

void RecordFill(VulkanBackend& backend, VulkanCommandContext& command_context,
                const VulkanBackend::Texture2D& texture, const fill::FillImagePushConstants& push) {
  const std::array<VulkanImageBinding, 1> images = {
      VulkanImageBinding{fill::kImageBinding, &texture}};
  RecordKernelDispatch(
      backend, command_context, fill::FillImageDescriptor(), images, PushConstantBytes(push),
      WorkgroupsCovering(kWidth, kHeight, fill::kFillImageGroupSize, fill::kFillImageGroupSize));
}

auto Download(VulkanBackend& backend, VulkanCommandContext& command_context,
              const VulkanBackend::Texture2D& texture) -> std::vector<float> {
  std::vector<float> pixels(static_cast<std::size_t>(texture.Width()) * texture.Height() * 4);
  backend.DownloadTexture2D(texture, std::as_writable_bytes(std::span<float>(pixels)),
                            command_context);
  return pixels;
}

void ExpectEachTexelEquals(const std::vector<float>& pixels, std::array<float, 4> value) {
  std::size_t wrong = 0;
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    if (pixels[i] != value[i % 4]) {
      ++wrong;
    }
  }
  EXPECT_EQ(wrong, 0U) << "of " << pixels.size() << " values";
}

TEST_F(VulkanWorkspaceFixture, DispatchWritesEachTexelAndCountsOneDispatch) {
  VulkanRenderDevice device;
  auto&              backend         = device.Workspace().Device();
  auto&              command_context = device.CommandContext();
  auto               texture = backend.CreateTexture2D(kWidth, kHeight, TextureFormat::Rgba32f);
  const std::array<float, 4> value  = {1.5F, -2.0F, 3.25F, 0.5F};

  const auto                 before = vulkan::ReadVulkanApiCounters();
  RecordFill(backend, command_context, texture, FillPush(value));
  EXPECT_TRUE(command_context.IsRecording());
  const auto delta = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
  EXPECT_EQ(delta.dispatches, 1U);
  EXPECT_EQ(delta.submits, 0U) << "a recorded dispatch is not submitted";

  ExpectEachTexelEquals(Download(backend, command_context, texture), value);
}

TEST_F(VulkanWorkspaceFixture, CopyAfterADispatchReadsTheWrittenTexels) {
  VulkanRenderDevice device;
  auto&              backend         = device.Workspace().Device();
  auto&              command_context = device.CommandContext();
  auto               written = backend.CreateTexture2D(kWidth, kHeight, TextureFormat::Rgba32f);
  auto               copy    = backend.CreateTexture2D(kWidth, kHeight, TextureFormat::Rgba32f);
  const std::array<float, 4> first  = {0.25F, 0.5F, 0.75F, 1.0F};
  const std::array<float, 4> second = {-4.0F, 8.0F, -16.0F, 32.0F};

  // One recording: the copy and the second dispatch must see the earlier writes (write barrier).
  RecordFill(backend, command_context, written, FillPush(first));
  backend.CopyTexture2D(written, copy, command_context);
  RecordFill(backend, command_context, written, FillPush(second));

  ExpectEachTexelEquals(Download(backend, command_context, copy), first);
  ExpectEachTexelEquals(Download(backend, command_context, written), second);
}

TEST_F(VulkanWorkspaceFixture, DispatchWithAnImageForAnotherBindingIsRejected) {
  VulkanRenderDevice device;
  auto&              backend         = device.Workspace().Device();
  auto&              command_context = device.CommandContext();
  auto               texture = backend.CreateTexture2D(kWidth, kHeight, TextureFormat::Rgba32f);
  const auto         push    = FillPush({1.0F, 1.0F, 1.0F, 1.0F});

  const auto         before  = vulkan::ReadVulkanApiCounters();
  const std::array<VulkanImageBinding, 1> other_binding = {VulkanImageBinding{3, &texture}};
  EXPECT_THROW(RecordKernelDispatch(backend, command_context, fill::FillImageDescriptor(),
                                    other_binding, PushConstantBytes(push), {5, 3, 1}),
               std::invalid_argument);
  const std::array<VulkanImageBinding, 1> empty_texture = {
      VulkanImageBinding{fill::kImageBinding, nullptr}};
  EXPECT_THROW(RecordKernelDispatch(backend, command_context, fill::FillImageDescriptor(),
                                    empty_texture, PushConstantBytes(push), {5, 3, 1}),
               std::invalid_argument);
  EXPECT_THROW(RecordKernelDispatch(backend, command_context, fill::FillImageDescriptor(), {},
                                    PushConstantBytes(push), {5, 3, 1}),
               std::invalid_argument);

  const auto delta = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
  EXPECT_EQ(delta.dispatches, 0U);
  EXPECT_EQ(delta.pipeline_creations + delta.pipeline_cache_hits, 0U)
      << "the checks come before the pipeline request";
  EXPECT_FALSE(command_context.IsRecording());
}

TEST_F(VulkanWorkspaceFixture, DispatchWithAnotherPushConstantSizeIsRejected) {
  VulkanRenderDevice device;
  auto&              backend         = device.Workspace().Device();
  auto&              command_context = device.CommandContext();
  auto               texture = backend.CreateTexture2D(kWidth, kHeight, TextureFormat::Rgba32f);
  const std::array<VulkanImageBinding, 1> images = {
      VulkanImageBinding{fill::kImageBinding, &texture}};
  const std::array<std::byte, 16> short_push{};

  const auto                      before = vulkan::ReadVulkanApiCounters();
  try {
    RecordKernelDispatch(backend, command_context, fill::FillImageDescriptor(), images, short_push,
                         {5, 3, 1});
    ADD_FAILURE() << "a dispatch with 16 push constant bytes for a kernel of 24 was recorded";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("test_fill_image"), std::string::npos) << error.what();
    EXPECT_NE(std::string(error.what()).find("24"), std::string::npos) << error.what();
  }
  const auto delta = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
  EXPECT_EQ(delta.dispatches, 0U);
  EXPECT_FALSE(command_context.IsRecording());
}

TEST_F(VulkanWorkspaceFixture, DispatchOfZeroWorkgroupsRecordsNoCommand) {
  VulkanRenderDevice device;
  auto&              backend         = device.Workspace().Device();
  auto&              command_context = device.CommandContext();
  auto               texture = backend.CreateTexture2D(kWidth, kHeight, TextureFormat::Rgba32f);
  const std::array<VulkanImageBinding, 1> images = {
      VulkanImageBinding{fill::kImageBinding, &texture}};
  const auto push   = FillPush({1.0F, 1.0F, 1.0F, 1.0F});

  const auto before = vulkan::ReadVulkanApiCounters();
  RecordKernelDispatch(backend, command_context, fill::FillImageDescriptor(), images,
                       PushConstantBytes(push), {0, 3, 1});
  const auto delta = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
  EXPECT_EQ(delta.dispatches, 0U);
  EXPECT_FALSE(command_context.IsRecording());
}

TEST_F(VulkanWorkspaceFixture, SecondDispatchOfOneKernelCreatesNoPipeline) {
  VulkanRenderDevice device;
  auto&              backend         = device.Workspace().Device();
  auto&              command_context = device.CommandContext();
  auto               texture = backend.CreateTexture2D(kWidth, kHeight, TextureFormat::Rgba32f);
  const std::array<float, 4> value = {7.0F, 6.0F, 5.0F, 4.0F};

  RecordFill(backend, command_context, texture, FillPush({0.0F, 0.0F, 0.0F, 0.0F}));
  const auto before = vulkan::ReadVulkanApiCounters();
  RecordFill(backend, command_context, texture, FillPush(value));
  const auto delta = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
  EXPECT_EQ(delta.pipeline_creations, 0U);
  EXPECT_EQ(delta.module_loads, 0U);
  EXPECT_EQ(delta.pipeline_cache_hits, 1U);
  EXPECT_EQ(delta.dispatches, 1U);

  ExpectEachTexelEquals(Download(backend, command_context, texture), value);
}

}  // namespace
}  // namespace alcedo
