//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// VulkanComputePipelineCache on the local GPU: one pipeline for each kernel and specialization,
// pipelines that run and write the requested values, the checks of a kernel descriptor, and the
// stored VkPipelineCache data.

#include "vulkan/vulkan_compute_pipeline_cache.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "vulkan/vulkan_api_counters.hpp"
#include "vulkan/vulkan_context.hpp"
#include "vulkan/vulkan_kernel_catalog.hpp"
#include "vulkan/vulkan_pipeline_cache_store.hpp"
#include "vulkan/vulkan_test_fill_kernels.hpp"
#include "vulkan_test_device.hpp"

namespace alcedo::vulkan {
namespace {

using test_support::HostVisibleBuffer;
using test_support::OptionsOfThisRun;
using test_support::StorageImage;
using test_support::SubmitAndWait;
using test_support::TemporaryFolder;

class VulkanComputePipelineCacheTest : public test_support::VulkanContextTestBase {
 protected:
  /// Creates the context of the run with @p cache_file as the pipeline cache file.
  auto CreateContext(std::filesystem::path cache_file = {}) -> VulkanContext& {
    auto options                = OptionsOfThisRun();
    options.pipeline_cache_file = std::move(cache_file);
    return VulkanContext::Create(options);
  }

  /// Runs test_fill with @p specialization on @p count elements and gives the buffer values.
  static auto RunFillBuffer(VulkanContext& context, uint32_t count, uint32_t value,
                            std::vector<VulkanSpecializationValue> specialization = {})
      -> std::vector<uint32_t> {
    const auto& pipeline = context.ComputePipelines().Pipeline(
        test_fill_kernels::FillBufferDescriptor(std::move(specialization)));
    HostVisibleBuffer buffer(context, count * sizeof(uint32_t));
    std::memset(buffer.Mapped(), 0xFF, count * sizeof(uint32_t));

    const auto& functions = context.PipelineDeviceFunctions();
    SubmitAndWait(context, [&](VkCommandBuffer command_buffer) {
      test_fill_kernels::FillBufferPushConstants push;
      push.output = buffer.Address();
      push.count  = count;
      push.value  = value;
      functions.vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  pipeline.pipeline);
      functions.vkCmdPushConstants(command_buffer, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(push), &push);
      const uint32_t groups = (count + test_fill_kernels::kFillBufferGroupSize - 1) /
                              test_fill_kernels::kFillBufferGroupSize;
      functions.vkCmdDispatch(command_buffer, groups, 1, 1);
      test_support::ComputeWriteBarrier(functions, command_buffer, VK_PIPELINE_STAGE_2_HOST_BIT,
                                        VK_ACCESS_2_HOST_READ_BIT);
    });
    buffer.InvalidateForHostRead();
    std::vector<uint32_t> values(count);
    std::memcpy(values.data(), buffer.Mapped(), count * sizeof(uint32_t));
    return values;
  }
};

TEST_F(VulkanComputePipelineCacheTest, SecondRequestForOneKernelCreatesNoPipeline) {
  auto&       cache        = CreateContext().ComputePipelines();
  const auto  before       = ReadVulkanApiCounters();

  const auto& first        = cache.Pipeline(test_fill_kernels::FillBufferDescriptor());
  const auto  after_first  = ReadVulkanApiCounters();
  const auto& second       = cache.Pipeline(test_fill_kernels::FillBufferDescriptor());
  const auto  after_second = ReadVulkanApiCounters();

  EXPECT_NE(first.pipeline, VK_NULL_HANDLE);
  EXPECT_NE(first.layout, VK_NULL_HANDLE);
  EXPECT_EQ(first.set_layout, VK_NULL_HANDLE) << "test_fill has no image binding.";
  EXPECT_EQ(&first, &second);
  EXPECT_EQ(VulkanApiCounterDelta(before, after_first).pipeline_creations, 1U);
  EXPECT_EQ(VulkanApiCounterDelta(before, after_first).module_loads, 1U);
  const auto second_delta = VulkanApiCounterDelta(after_first, after_second);
  EXPECT_EQ(second_delta.pipeline_creations, 0U);
  EXPECT_EQ(second_delta.module_loads, 0U);
  EXPECT_EQ(second_delta.pipeline_cache_hits, 1U);
}

TEST_F(VulkanComputePipelineCacheTest, FillKernelWritesTheRequestedValueToEachElement) {
  auto&      context = CreateContext();
  // 1000 is not a multiple of the workgroup size: the last group checks the count.
  const auto values  = RunFillBuffer(context, 1000, 0x5A17U);
  ASSERT_EQ(values.size(), 1000U);
  for (std::size_t i = 0; i < values.size(); ++i) {
    ASSERT_EQ(values[i], 0x5A17U) << "element " << i;
  }
}

TEST_F(VulkanComputePipelineCacheTest, SpecializationValueGivesASecondPipelineThatAddsTheValue) {
  auto&                                        context = CreateContext();
  auto&                                        cache   = context.ComputePipelines();
  const auto                                   before  = ReadVulkanApiCounters();
  const std::vector<VulkanSpecializationValue> addend = {{test_fill_kernels::kAddendConstantId, 5}};

  const auto& plain       = cache.Pipeline(test_fill_kernels::FillBufferDescriptor());
  const auto& specialized = cache.Pipeline(test_fill_kernels::FillBufferDescriptor(addend));
  EXPECT_NE(plain.pipeline, specialized.pipeline);
  EXPECT_EQ(VulkanApiCounterDelta(before, ReadVulkanApiCounters()).pipeline_creations, 2U);

  const auto values = RunFillBuffer(context, 64, 100, addend);
  for (const uint32_t value : values) {
    ASSERT_EQ(value, 105U);
  }
  EXPECT_EQ(RunFillBuffer(context, 64, 100).front(), 100U);
  EXPECT_EQ(VulkanApiCounterDelta(before, ReadVulkanApiCounters()).pipeline_creations, 2U);
}

TEST_F(VulkanComputePipelineCacheTest, ImageFillKernelWritesTheRequestedValueToEachTexel) {
  auto&       context = CreateContext();
  const auto& pipeline =
      context.ComputePipelines().Pipeline(test_fill_kernels::FillImageDescriptor());
  ASSERT_NE(pipeline.set_layout, VK_NULL_HANDLE);

  constexpr uint32_t kWidth  = 37;
  constexpr uint32_t kHeight = 21;
  StorageImage       image(context, kWidth, kHeight);
  HostVisibleBuffer  readback(context, kWidth * kHeight * 4 * sizeof(float));
  const auto&        functions = context.PipelineDeviceFunctions();

  SubmitAndWait(context, [&](VkCommandBuffer command_buffer) {
    test_support::TransitionImage(
        functions, command_buffer, image.Image(), VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    functions.vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
    VkDescriptorImageInfo image_info{};
    image_info.imageView   = image.View();
    image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstBinding      = test_fill_kernels::kImageBinding;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo      = &image_info;
    functions.vkCmdPushDescriptorSetKHR(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        pipeline.layout, 0, 1, &write);
    test_fill_kernels::FillImagePushConstants push;
    push.value[0] = 0.25F;
    push.value[1] = -1.5F;
    push.value[2] = 3.0F;
    push.value[3] = 1.0F;
    push.width    = kWidth;
    push.height   = kHeight;
    functions.vkCmdPushConstants(command_buffer, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                 sizeof(push), &push);
    constexpr uint32_t kGroup = test_fill_kernels::kFillImageGroupSize;
    functions.vkCmdDispatch(command_buffer, (kWidth + kGroup - 1) / kGroup,
                            (kHeight + kGroup - 1) / kGroup, 1);

    test_support::TransitionImage(functions, command_buffer, image.Image(), VK_IMAGE_LAYOUT_GENERAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {kWidth, kHeight, 1};
    functions.vkCmdCopyImageToBuffer(command_buffer, image.Image(),
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.Buffer(), 1,
                                     &region);
    VkMemoryBarrier2 to_host{};
    to_host.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    to_host.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_host.dstStageMask  = VK_PIPELINE_STAGE_2_HOST_BIT;
    to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{};
    dependency.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers    = &to_host;
    functions.vkCmdPipelineBarrier2(command_buffer, &dependency);
  });

  readback.InvalidateForHostRead();
  const auto* texels = static_cast<const float*>(readback.Mapped());
  for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
    ASSERT_EQ(texels[4 * i + 0], 0.25F) << "texel " << i;
    ASSERT_EQ(texels[4 * i + 1], -1.5F) << "texel " << i;
    ASSERT_EQ(texels[4 * i + 2], 3.0F) << "texel " << i;
    ASSERT_EQ(texels[4 * i + 3], 1.0F) << "texel " << i;
  }
}

TEST_F(VulkanComputePipelineCacheTest, BufferBindingIsRejectedAndCreatesNoPipeline) {
  auto& cache                      = CreateContext().ComputePipelines();
  auto  descriptor                 = test_fill_kernels::FillImageDescriptor();
  descriptor.bindings.front().type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  const auto before                = ReadVulkanApiCounters();
  EXPECT_THROW((void)cache.Pipeline(descriptor), std::invalid_argument);
  const auto delta = VulkanApiCounterDelta(before, ReadVulkanApiCounters());
  EXPECT_EQ(delta.pipeline_creations, 0U);
  EXPECT_EQ(delta.module_loads, 0U);
}

TEST_F(VulkanComputePipelineCacheTest, OtherPushConstantSizeForAKnownKernelIsRejected) {
  auto& cache = CreateContext().ComputePipelines();
  (void)cache.Pipeline(test_fill_kernels::FillBufferDescriptor());
  auto descriptor =
      test_fill_kernels::FillBufferDescriptor({{test_fill_kernels::kAddendConstantId, 1}});
  descriptor.push_constant_bytes = 8;
  EXPECT_THROW((void)cache.Pipeline(descriptor), std::logic_error);
}

TEST_F(VulkanComputePipelineCacheTest, AbsentKernelLeavesNoEntryAndCreatesNoPipeline) {
  auto&      cache  = CreateContext().ComputePipelines();
  const auto before = ReadVulkanApiCounters();
  EXPECT_THROW((void)cache.Pipeline({"absent_kernel", {}, 0, {}}), std::runtime_error);
  EXPECT_EQ(VulkanApiCounterDelta(before, ReadVulkanApiCounters()).pipeline_creations, 0U);
}

TEST_F(VulkanComputePipelineCacheTest, EachCatalogKernelHasAPipeline) {
  auto& context = CreateContext();
  for (const auto& descriptor : VulkanKernelCatalog()) {
    EXPECT_NE(context.ComputePipelines().Pipeline(descriptor).pipeline, VK_NULL_HANDLE)
        << descriptor.kernel;
  }
  auto catalog_names = std::vector<std::string>{};
  for (const auto& descriptor : VulkanKernelCatalog()) {
    catalog_names.push_back(descriptor.kernel);
  }
  std::sort(catalog_names.begin(), catalog_names.end());
  EXPECT_EQ(catalog_names, context.ShaderModules().KernelNames());
}

// --- Stored VkPipelineCache data --------------------------------------------------------------

/// Pipeline cache data with a VkPipelineCacheHeaderVersionOne of @p identity and some payload.
auto PipelineCacheDataOf(const VulkanPipelineCacheIdentity& identity) -> std::vector<std::byte> {
  std::vector<std::byte> data(16 + VK_UUID_SIZE + 8, std::byte{0x3C});
  const uint32_t         header[4] = {static_cast<uint32_t>(16 + VK_UUID_SIZE),
                                      static_cast<uint32_t>(VK_PIPELINE_CACHE_HEADER_VERSION_ONE),
                                      identity.vendor_id, identity.device_id};
  std::memcpy(data.data(), header, sizeof(header));
  std::memcpy(data.data() + 16, identity.pipeline_cache_uuid.data(), VK_UUID_SIZE);
  return data;
}

auto IdentityOf(const VulkanContext& context) -> VulkanPipelineCacheIdentity {
  VulkanPipelineCacheIdentity identity;
  identity.vendor_id           = context.Capabilities().vendor_id;
  identity.device_id           = context.Capabilities().device_id;
  identity.pipeline_cache_uuid = context.Capabilities().pipeline_cache_uuid;
  return identity;
}

TEST_F(VulkanComputePipelineCacheTest, StoredCacheDataFromADifferentDriverIsReplaced) {
  TemporaryFolder folder("pipeline_cache_other_driver");
  const auto      cache_file = folder.Path() / "vulkan_pipeline_cache.bin";
  {
    // A file of the same device with another driver: the cache UUID differs.
    auto& probe = CreateContext();
    auto  other = IdentityOf(probe);
    other.pipeline_cache_uuid[0] ^= 0xFFU;
    VulkanContext::Destroy();
    WriteStoredPipelineCacheData(cache_file, PipelineCacheDataOf(other));
  }

  auto& context = CreateContext(cache_file);
  EXPECT_EQ(context.ComputePipelines().StoredDataStatus(),
            VulkanStoredPipelineCacheStatus::DifferentDriver);
  (void)context.ComputePipelines().Pipeline(test_fill_kernels::FillBufferDescriptor());
  const auto identity = IdentityOf(context);
  VulkanContext::Destroy();  // stores the pipeline cache data

  const auto stored = ReadStoredPipelineCacheData(cache_file, identity);
  ASSERT_EQ(stored.status, VulkanStoredPipelineCacheStatus::Loaded) << stored.detail;
  const auto stored_identity = ReadPipelineCacheDataIdentity(stored.data);
  ASSERT_TRUE(stored_identity.has_value());
  EXPECT_EQ(stored_identity->pipeline_cache_uuid, identity.pipeline_cache_uuid);

  // The next context of this device starts with the stored data.
  auto& next = CreateContext(cache_file);
  EXPECT_EQ(next.ComputePipelines().StoredDataStatus(), VulkanStoredPipelineCacheStatus::Loaded);
}

TEST_F(VulkanComputePipelineCacheTest, DamagedCacheFileIsReportedAndNotUsed) {
  TemporaryFolder folder("pipeline_cache_damaged");
  const auto      cache_file = folder.Path() / "vulkan_pipeline_cache.bin";
  auto&           probe      = CreateContext();
  const auto      identity   = IdentityOf(probe);
  VulkanContext::Destroy();

  WriteStoredPipelineCacheData(cache_file, PipelineCacheDataOf(identity));
  ASSERT_EQ(ReadStoredPipelineCacheData(cache_file, identity).status,
            VulkanStoredPipelineCacheStatus::Loaded);
  {
    // One changed byte of the data: the SHA-256 of the header no longer agrees.
    std::fstream stream(cache_file, std::ios::binary | std::ios::in | std::ios::out);
    stream.seekp(-1, std::ios::end);
    stream.put('\x7E');
  }
  const auto damaged = ReadStoredPipelineCacheData(cache_file, identity);
  EXPECT_EQ(damaged.status, VulkanStoredPipelineCacheStatus::Damaged);
  EXPECT_TRUE(damaged.data.empty());

  auto& context = CreateContext(cache_file);
  EXPECT_EQ(context.ComputePipelines().StoredDataStatus(),
            VulkanStoredPipelineCacheStatus::Damaged);
  EXPECT_NE(context.ComputePipelines().Pipeline(test_fill_kernels::FillBufferDescriptor()).pipeline,
            VK_NULL_HANDLE);
}

TEST(VulkanPipelineCacheStoreTest, AbsentFileGivesNoFileAndNoData) {
  TemporaryFolder folder("pipeline_cache_absent");
  const auto      stored = ReadStoredPipelineCacheData(folder.Path() / "absent.bin", {});
  EXPECT_EQ(stored.status, VulkanStoredPipelineCacheStatus::NoFile);
  EXPECT_TRUE(stored.data.empty());
}

}  // namespace
}  // namespace alcedo::vulkan
