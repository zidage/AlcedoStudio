//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// VulkanBackend, VulkanCommandContext, and the shared device and workspace templates on the local
// GPU (plan phase VK2). Each test runs with the validation layer; the fixture fails a test that
// gets a validation error. Set ALCEDO_VULKAN_DEVICE (for example "Iris") to select the device.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "edit/graph/graph_ids.hpp"
#include "edit/runtime/render_backend.hpp"
#include "edit/runtime/result_representation.hpp"
#include "edit/runtime/runtime_revision.hpp"
#include "edit/runtime/texture_format.hpp"
#include "vulkan/vulkan_api_counters.hpp"
#include "vulkan_workspace_test_support.hpp"

namespace alcedo {
namespace {

using vulkan_workspace_test::BindSharpen;
using vulkan_workspace_test::ExposureFieldBindings;
using vulkan_workspace_test::RampRgba32f;
using vulkan_workspace_test::UploadPackedModel;
using vulkan_workspace_test::VulkanWorkspaceFixture;
using vulkan_workspace_test::WritePackedOwnerBytes;

template <class T>
auto AsBytes(const std::vector<T>& values) -> std::span<const std::byte> {
  return std::as_bytes(std::span<const T>(values));
}

template <class T>
auto AsWritableBytes(std::vector<T>& values) -> std::span<std::byte> {
  return std::as_writable_bytes(std::span<T>(values));
}

TEST_F(VulkanWorkspaceFixture, RenderDeviceTemplatesInstantiateForVulkan) {
  static_assert(RenderBackend<VulkanBackend>);
  static_assert(!std::is_copy_constructible_v<VulkanBackend::Texture2D>);
  static_assert(std::is_nothrow_move_constructible_v<VulkanBackend::Texture2D>);
  static_assert(std::is_nothrow_move_constructible_v<VulkanBackend::Buffer>);
  EXPECT_STREQ(VulkanBackend::kName, "Vulkan");

  VulkanRenderDevice device;
  EXPECT_FALSE(device.Workspace().IsRendering());
  device.BeginRender();
  EXPECT_EQ(device.CommandContext().SubmissionId(), 1U);
  device.EndRender();
  device.WaitIdle();
  EXPECT_EQ(device.Workspace().Device().CompletedSubmission(), 1U);
  EXPECT_FALSE(device.Workspace().Device().HasInFlightSubmission());
}

TEST_F(VulkanWorkspaceFixture, UploadedRgba32fImageDownloadsWithEqualValues) {
  VulkanRenderDevice      device;
  auto&                   backend = device.Workspace().Device();
  // An odd size: rows are packed without padding in both directions.
  constexpr std::uint32_t kWidth  = 37;
  constexpr std::uint32_t kHeight = 23;
  const auto              pixels  = RampRgba32f(kWidth, kHeight, 0.0625F);

  device.BeginRender();
  auto texture = device.Workspace().Textures().Acquire({kWidth, kHeight, TextureFormat::Rgba32f});
  backend.UploadTexture2D(texture.Texture(), AsBytes(pixels), device.CommandContext());
  device.EndRender();
  device.WaitIdle();

  std::vector<float> readback(pixels.size(), -1.0F);
  backend.DownloadTexture2D(texture.Texture(), AsWritableBytes(readback), device.CommandContext());
  EXPECT_EQ(readback, pixels);
  EXPECT_EQ(backend.HostToDeviceBytes(), pixels.size() * sizeof(float));
  EXPECT_EQ(backend.DeviceToHostCopyCount(), 1U);
}

TEST_F(VulkanWorkspaceFixture, R32fAndR8ImagesDownloadWithEqualValues) {
  VulkanRenderDevice device;
  auto&              backend  = device.Workspace().Device();
  auto&              commands = device.CommandContext();

  auto               r32      = backend.CreateTexture2D(9, 5, TextureFormat::R32f);
  std::vector<float> r32_values(45);
  for (std::size_t i = 0; i < r32_values.size(); ++i) {
    r32_values[i] = 0.5F * static_cast<float>(i) - 7.0F;
  }
  auto                   r8 = backend.CreateTexture2D(13, 3, TextureFormat::R8);
  std::vector<std::byte> r8_values(39);
  for (std::size_t i = 0; i < r8_values.size(); ++i) {
    r8_values[i] = static_cast<std::byte>(i * 6);
  }
  backend.UploadTexture2D(r32, AsBytes(r32_values), commands);
  backend.UploadTexture2D(r8, r8_values, commands);

  std::vector<float>     r32_back(r32_values.size());
  std::vector<std::byte> r8_back(r8_values.size());
  backend.DownloadTexture2D(r32, AsWritableBytes(r32_back), commands);
  backend.DownloadTexture2D(r8, r8_back, commands);
  EXPECT_EQ(r32_back, r32_values);
  EXPECT_EQ(r8_back, r8_values);
}

TEST_F(VulkanWorkspaceFixture, R8DirtyRectangleUploadChangesOnlyTheRectangle) {
  VulkanRenderDevice      device;
  auto&                   backend  = device.Workspace().Device();
  auto&                   commands = device.CommandContext();
  constexpr std::uint32_t kWidth   = 24;
  constexpr std::uint32_t kHeight  = 16;
  auto                    mask     = backend.CreateTexture2D(kWidth, kHeight, TextureFormat::R8);
  backend.UploadTexture2D(mask, std::vector<std::byte>(kWidth * kHeight, std::byte{0x10}),
                          commands);
  device.WaitIdle();

  backend.ResetCounters();
  const RectI rectangle{5, 3, 7, 4};
  backend.UploadR8TextureRect(mask, rectangle, std::vector<std::byte>(28, std::byte{0xF0}),
                              commands);
  std::vector<std::byte> readback(kWidth * kHeight);
  backend.DownloadTexture2D(mask, readback, commands);

  EXPECT_EQ(backend.HostToDeviceBytes(), 28U);
  ASSERT_EQ(backend.LastTextureRectangles().size(), 1U);
  EXPECT_EQ(backend.LastTextureRectangles().front().width, 7);
  for (std::uint32_t y = 0; y < kHeight; ++y) {
    for (std::uint32_t x = 0; x < kWidth; ++x) {
      const bool inside = static_cast<std::int32_t>(x) >= rectangle.x &&
                          static_cast<std::int32_t>(x) < rectangle.X1() &&
                          static_cast<std::int32_t>(y) >= rectangle.y &&
                          static_cast<std::int32_t>(y) < rectangle.Y1();
      EXPECT_EQ(readback[y * kWidth + x], inside ? std::byte{0xF0} : std::byte{0x10})
          << "x=" << x << " y=" << y;
    }
  }
}

TEST_F(VulkanWorkspaceFixture, BufferRangeUploadsAndDownloadsAtItsOffset) {
  VulkanRenderDevice device;
  auto&              backend  = device.Workspace().Device();
  auto&              commands = device.CommandContext();
  auto               buffer   = backend.CreateBuffer(1024);
  EXPECT_NE(buffer.DeviceAddress(), 0U);
  backend.UploadBufferRange(buffer, 0, std::vector<std::byte>(1024, std::byte{0}), commands);
  backend.UploadBufferRange(buffer, 256, std::vector<std::byte>(100, std::byte{0x7C}), commands);

  std::vector<std::byte> readback(1024);
  backend.DownloadBufferRange(buffer, 0, readback, commands);
  for (std::size_t i = 0; i < readback.size(); ++i) {
    const bool inside = i >= 256 && i < 356;
    EXPECT_EQ(readback[i], inside ? std::byte{0x7C} : std::byte{0}) << "byte " << i;
  }
  std::vector<std::byte> range(100);
  backend.DownloadBufferRange(buffer, 256, range, commands);
  EXPECT_EQ(range, std::vector<std::byte>(100, std::byte{0x7C}));
}

TEST_F(VulkanWorkspaceFixture, BufferFillWritesTheValueOnlyInItsRange) {
  VulkanRenderDevice device;
  auto&              backend  = device.Workspace().Device();
  auto&              commands = device.CommandContext();
  auto               buffer   = backend.CreateBuffer(256);
  backend.UploadBufferRange(buffer, 0, std::vector<std::byte>(256, std::byte{0x5A}), commands);
  backend.FillBufferRange(buffer, 64, 24, 0, commands);
  backend.FillBufferRange(buffer, 128, 8, 0x3F800000U, commands);  // 1.0F

  std::vector<std::byte> readback(256);
  backend.DownloadBufferRange(buffer, 0, readback, commands);
  for (std::size_t i = 0; i < readback.size(); ++i) {
    std::byte expected = std::byte{0x5A};
    if (i >= 64 && i < 88) {
      expected = std::byte{0};
    } else if (i >= 128 && i < 136) {
      expected = (i % 4 == 3) ? std::byte{0x3F} : (i % 4 == 2 ? std::byte{0x80} : std::byte{0});
    }
    EXPECT_EQ(readback[i], expected) << "byte " << i;
  }
  EXPECT_THROW(backend.FillBufferRange(buffer, 2, 8, 0, commands), std::runtime_error);
  EXPECT_THROW(backend.FillBufferRange(buffer, 0, 6, 0, commands), std::runtime_error);
  EXPECT_THROW(backend.FillBufferRange(buffer, 252, 8, 0, commands), std::runtime_error);
}

TEST_F(VulkanWorkspaceFixture, TextureRegionCopyKeepsTheTexelsOfTheRectangle) {
  VulkanRenderDevice      device;
  auto&                   backend  = device.Workspace().Device();
  auto&                   commands = device.CommandContext();
  constexpr std::uint32_t kWidth   = 21;
  constexpr std::uint32_t kHeight  = 13;
  const auto              pixels   = RampRgba32f(kWidth, kHeight, 0.125F);
  const RectI             rectangle{3, 2, 11, 7};
  auto                    source = backend.CreateTexture2D(kWidth, kHeight, TextureFormat::Rgba32f);
  auto                    region = backend.CreateTexture2D(11, 7, TextureFormat::Rgba32f);
  backend.UploadTexture2D(source, AsBytes(pixels), commands);
  backend.CopyTexture2DRegion(source, rectangle, region, commands);

  std::vector<float> readback(std::size_t{11} * 7 * 4);
  backend.DownloadTexture2D(region, AsWritableBytes(readback), commands);
  for (std::uint32_t y = 0; y < 7; ++y) {
    for (std::uint32_t x = 0; x < 11; ++x) {
      for (std::uint32_t c = 0; c < 4; ++c) {
        const std::size_t from = ((y + 2) * kWidth + (x + 3)) * 4 + c;
        ASSERT_EQ(readback[(y * 11 + x) * 4 + c], pixels[from]) << "x=" << x << " y=" << y;
      }
    }
  }
  auto r32f = backend.CreateTexture2D(11, 7, TextureFormat::R32f);
  EXPECT_THROW(backend.CopyTexture2DRegion(source, RectI{11, 2, 11, 7}, region, commands),
               std::runtime_error);
  EXPECT_THROW(backend.CopyTexture2DRegion(source, RectI{3, 2, 10, 7}, region, commands),
               std::runtime_error);
  EXPECT_THROW(backend.CopyTexture2DRegion(source, rectangle, r32f, commands), std::runtime_error);
}

TEST_F(VulkanWorkspaceFixture, ImageCopiedThroughABufferAndAnImageKeepsEveryTexel) {
  VulkanRenderDevice      device;
  auto&                   backend = device.Workspace().Device();
  constexpr std::uint32_t kWidth  = 19;
  constexpr std::uint32_t kHeight = 11;
  constexpr std::uint32_t kOffset = 64;
  const auto              pixels  = RampRgba32f(kWidth, kHeight, 0.25F);
  const std::size_t       bytes   = pixels.size() * sizeof(float);
  auto                    source = backend.CreateTexture2D(kWidth, kHeight, TextureFormat::Rgba32f);
  auto through                   = backend.CreateTexture2D(kWidth, kHeight, TextureFormat::Rgba32f);
  auto destination               = backend.CreateTexture2D(kWidth, kHeight, TextureFormat::Rgba32f);
  auto staging                   = backend.CreateBuffer(kOffset + bytes);

  // One render records every copy; each copy reads the result of the one before it.
  device.BeginRender();
  backend.UploadTexture2D(source, AsBytes(pixels), device.CommandContext());
  backend.CopyImageToBuffer(source, staging, kOffset, device.CommandContext());
  backend.CopyBufferToImage(staging, kOffset, through, device.CommandContext());
  backend.CopyTexture2D(through, destination, device.CommandContext());
  device.EndRender();
  device.WaitIdle();
  EXPECT_EQ(backend.SubmitCount(), 1U);

  std::vector<float> readback(pixels.size());
  backend.DownloadTexture2D(destination, AsWritableBytes(readback), device.CommandContext());
  EXPECT_EQ(readback, pixels);
}

TEST_F(VulkanWorkspaceFixture, HostUploadKeepsItsBytesAfterTheCallerReusesTheStorage) {
  VulkanRenderDevice device;
  auto&              backend  = device.Workspace().Device();
  auto&              commands = device.CommandContext();
  auto               buffer   = backend.CreateBuffer(128);
  auto               rgba     = backend.CreateTexture2D(2, 4, TextureFormat::Rgba32f);
  {
    std::vector<std::byte> source(128, std::byte{0x25});
    backend.UploadBufferRange(buffer, 0, source, commands);
    backend.UploadTexture2D(rgba, source, commands);
    // The commands are recorded and not submitted; the caller overwrites its array.
    std::fill(source.begin(), source.end(), std::byte{0x6B});
  }
  EXPECT_TRUE(commands.IsRecording());
  std::vector<std::byte> readback(128);
  backend.DownloadBufferRange(buffer, 0, readback, commands);
  EXPECT_EQ(readback, std::vector<std::byte>(128, std::byte{0x25}));
  backend.DownloadTexture2D(rgba, readback, commands);
  EXPECT_EQ(readback, std::vector<std::byte>(128, std::byte{0x25}));
}

TEST_F(VulkanWorkspaceFixture, ParameterArenaUploadsTheBoundSlotOnce) {
  VulkanRenderDevice device;
  ParameterSlotKey   key{NodeId{"grade.primary"}, AdjustmentInstanceId{"sharpen"}};
  SharpenModel       model;
  BindSharpen(device.Workspace().Parameters(), key);
  ASSERT_TRUE(UploadPackedModel(device, key, model));

  model.SetAmount(12.0F);
  auto& backend = device.Workspace().Device();
  backend.ResetCounters();
  WritePackedOwnerBytes(device.Workspace().Parameters(), key, model);
  device.Workspace().Parameters().UploadDirty(device.CommandContext());
  device.WaitIdle();

  ASSERT_EQ(backend.LastHostToDeviceRanges().size(), 1U);
  EXPECT_EQ(backend.LastHostToDeviceRanges().front().size, sizeof(SharpenPayload));
  EXPECT_EQ(backend.HostToDeviceBytes(), sizeof(SharpenPayload));

  backend.ResetCounters();
  device.Workspace().Parameters().UploadDirty(device.CommandContext());
  EXPECT_EQ(backend.HostToDeviceBytes(), 0U);
  EXPECT_EQ(backend.HostToDeviceCopyCount(), 0U);
}

TEST_F(VulkanWorkspaceFixture, TransientArenaRewindsToTheSameDeviceAddress) {
  VulkanRenderDevice device;
  auto&              transients = device.Workspace().TransientBuffers();
  transients.Reserve(4096);
  device.Workspace().Device().ResetCounters();

  void* first = transients.Allocate(512);
  ASSERT_NE(first, nullptr);
  transients.Reset();
  void* second = transients.Allocate(512);
  EXPECT_EQ(second, first);
  EXPECT_EQ(device.Workspace().Device().MallocCount(), 0U);
  EXPECT_EQ(device.Workspace().Device().BufferCreateCount(), 0U);
}

TEST_F(VulkanWorkspaceFixture, BufferAboveTheMaxSlabSizeIsRejectedWithTheLimit) {
  VulkanRenderDevice device;
  auto&              backend = device.Workspace().Device();
  EXPECT_GT(backend.MaxSlabBytes(), std::size_t{1} << 20);
  backend.SetMaxSlabBytes(4096);
  try {
    (void)backend.CreateBuffer(8192);
    FAIL() << "CreateBuffer above the max slab size did not throw";
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("8192"), std::string::npos) << message;
    EXPECT_NE(message.find("4096"), std::string::npos) << message;
  }
  EXPECT_EQ(backend.BufferCreateCount(), 0U);
}

TEST_F(VulkanWorkspaceFixture, SecondRenderOfOnePlanAllocatesNoImageAndNoBuffer) {
  VulkanRenderDevice device;
  auto&              workspace = device.Workspace();
  auto&              backend   = workspace.Device();
  ParameterSlotKey   key{NodeId{"grade.primary"}, AdjustmentInstanceId{"exposure"}};
  ExposureModel      model;
  workspace.Parameters().BindSlot(key, 4, ExposureFieldBindings());
  workspace.TransientBuffers().Reserve(1 << 20);
  const std::vector<float> pixels(64 * 64, 0.5F);

  // One render of the plan: a parameter upload, a pooled image with an upload, a transient range,
  // and a stored value.
  const auto               render = [&](float exposure) {
    model.SetValue(exposure);
    device.BeginRender();
    WritePackedOwnerBytes(workspace.Parameters(), key, model);
    workspace.Parameters().UploadDirty(device.CommandContext());
    auto texture = workspace.Textures().Acquire({64, 64, TextureFormat::R32f});
    backend.UploadTexture2D(texture.Texture(), AsBytes(pixels), device.CommandContext());
    ASSERT_NE(workspace.TransientBuffers().Allocate(2048), nullptr);
    device.EndRender();
  };

  render(0.25F);
  workspace.Values().Store({NodeId{"grade.primary"}, PortId{"commands"}}, backend.CreateBuffer(64));
  device.WaitIdle();
  backend.ResetCounters();
  device.CommandContext().ResetCounters();
  const auto before = vulkan::ReadVulkanApiCounters();

  render(0.5F);
  device.WaitIdle();

  const auto delta = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
  EXPECT_EQ(backend.BufferCreateCount(), 0U);
  EXPECT_EQ(backend.TextureCreateCount(), 0U);
  EXPECT_EQ(backend.MallocCount(), 0U);
  EXPECT_EQ(backend.FreeCount(), 0U);
  EXPECT_EQ(backend.SubmitCount(), 1U);
  EXPECT_EQ(device.CommandContext().CommandBufferCreateCount(), 0U);
  EXPECT_EQ(device.CommandContext().FenceCreateCount(), 0U);
  EXPECT_EQ(device.CommandContext().StagingBufferCreateCount(), 0U);
  EXPECT_EQ(delta.buffer_creations, 0U);
  EXPECT_EQ(delta.image_creations, 0U);
  EXPECT_EQ(delta.destroyed_resources, 0U);
  EXPECT_EQ(delta.submits, 1U);
}

TEST_F(VulkanWorkspaceFixture, TextureInAnUnfinishedSubmissionIsNotReleased) {
  VulkanRenderDevice      device;
  auto&                   textures = device.Workspace().Textures();
  auto&                   backend  = device.Workspace().Device();
  constexpr std::uint32_t kSize    = 64;

  device.BeginRender();
  auto       lease  = textures.Acquire({kSize, kSize, TextureFormat::R8});
  const auto handle = lease.Handle();
  backend.UploadTexture2D(lease.Texture(), std::vector<std::byte>(kSize * kSize, std::byte{3}),
                          device.CommandContext());
  device.EndRender();

  // The submission is not waited: the pool keeps the image although no lease holds it.
  lease.Release();
  textures.ReleaseUnleased();
  EXPECT_TRUE(textures.Contains(handle));
  EXPECT_TRUE(backend.IsResourceBusy(device.CommandContext().SubmissionId()));

  device.BeginRender();  // waits for the earlier submission
  const auto live_before = backend.AllocationTotals().live_allocations;
  textures.ReleaseUnleased();
  EXPECT_FALSE(textures.Contains(handle));
  EXPECT_EQ(backend.AllocationTotals().live_allocations, live_before - 1);
  device.EndRender();
  device.WaitIdle();
}

TEST_F(VulkanWorkspaceFixture, ReleasedTextureStaysAliveUntilItsBatchIsComplete) {
  VulkanRenderDevice device;
  auto&              backend       = device.Workspace().Device();
  auto&              commands      = device.CommandContext();
  const auto         live_at_start = backend.AllocationTotals().live_allocations;

  auto               texture       = backend.CreateTexture2D(32, 32, TextureFormat::Rgba32f);
  backend.UploadTexture2D(texture, std::vector<std::byte>(32 * 32 * 16, std::byte{1}), commands);
  ASSERT_TRUE(commands.IsRecording());
  // The open recording uses the image: Reset gives it back but does not destroy it.
  texture.Reset();
  EXPECT_EQ(backend.RetiredResourceCount(), 1U);
  EXPECT_EQ(backend.AllocationTotals().live_allocations, live_at_start + 1);

  backend.Submit(commands);
  EXPECT_EQ(backend.RetiredResourceCount(), 1U);
  backend.Wait(commands);
  EXPECT_EQ(backend.RetiredResourceCount(), 0U);
  EXPECT_EQ(backend.AllocationTotals().live_allocations, live_at_start);

  // A resource that no batch can use is destroyed at once.
  auto idle = backend.CreateBuffer(256);
  idle.Reset();
  EXPECT_EQ(backend.RetiredResourceCount(), 0U);
  EXPECT_EQ(backend.AllocationTotals().live_allocations, live_at_start);
}

TEST_F(VulkanWorkspaceFixture, AllocationAboveTheBudgetReportsSizeAndBudget) {
  VulkanRenderDevice device;
  auto&              backend = device.Workspace().Device();

  // The device budget: a buffer of the whole budget is above the budget minus the reserve.
  const auto         memory  = backend.QueryDeviceMemory();
  ASSERT_TRUE(memory.valid);
  const std::size_t whole_budget = memory.total_bytes;
  try {
    (void)backend.CreateBuffer(whole_budget);
    FAIL() << "A buffer of the whole budget did not throw";
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find(std::to_string(whole_budget) + " bytes"), std::string::npos) << message;
    EXPECT_NE(message.find("budget "), std::string::npos) << message;
    EXPECT_NE(message.find(std::to_string(VulkanBackend::kBudgetReserveBytes)), std::string::npos)
        << message;
  }

  // The application limit.
  backend.SetMemoryLimitBytes(std::size_t{1} << 20);
  try {
    (void)backend.CreateTexture2D(1024, 1024, TextureFormat::Rgba32f);
    FAIL() << "A texture above the application limit did not throw";
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("16777216"), std::string::npos) << message;
    EXPECT_NE(message.find("1048576"), std::string::npos) << message;
  }
  EXPECT_EQ(backend.MallocCount(), 0U);
  EXPECT_EQ(backend.AllocationTotals().live_allocations, 0U);

  // Below the limit the allocation works.
  auto small = backend.CreateTexture2D(64, 64, TextureFormat::Rgba32f);
  EXPECT_FALSE(small.Empty());
}

TEST_F(VulkanWorkspaceFixture, FailedEncodeDoesNotAdvanceTheCompletedSubmission) {
  VulkanRenderDevice        device;
  auto&                     workspace = device.Workspace();
  const GraphValueId        id{NodeId{"develop"}, PortId{"sensor_linear"}};
  constexpr RuntimeRevision first       = 41;
  constexpr RuntimeRevision second      = 42;
  const auto             representation = MakeExtentRepresentation({8, 8}, TextureFormat::Rgba32f);
  std::vector<std::byte> pixels(8 * 8 * 16, std::byte{1});

  device.BeginRender();
  (void)workspace.AcquireImageForWrite(id, {8, 8, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(id, first, representation,
                                       device.CommandContext().SubmissionId());
  workspace.Device().UploadTexture2D(workspace.Images().Find(id)->Texture(), pixels,
                                     device.CommandContext());
  device.EndRender();
  device.PublishResults();
  device.WaitIdle();
  const auto completed = workspace.Device().CompletedSubmission();
  ASSERT_TRUE(workspace.Images().FindValidResult(id, first, representation, completed));

  // The second encode fails at its upload and is cancelled.
  device.BeginRender();
  const auto failed_submission = device.CommandContext().SubmissionId();
  (void)workspace.AcquireImageForWrite(id, {8, 8, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(id, second, representation, failed_submission);
  workspace.Device().FailNextUpload();
  EXPECT_THROW(workspace.Device().UploadTexture2D(workspace.Images().Find(id)->Texture(), pixels,
                                                  device.CommandContext()),
               std::runtime_error);
  device.CancelRender();
  device.WaitIdle();

  EXPECT_EQ(workspace.Device().CompletedSubmission(), completed);
  EXPECT_LT(workspace.Device().CompletedSubmission(), failed_submission);
  EXPECT_TRUE(workspace.Images().FindValidResult(id, first, representation,
                                                 workspace.Device().CompletedSubmission()));
  EXPECT_FALSE(workspace.Images().FindValidResult(id, second, representation,
                                                  workspace.Device().CompletedSubmission()));
  EXPECT_EQ(workspace.Images().UnpublishedCount(), 0U);
}

TEST_F(VulkanWorkspaceFixture, FailedParameterUploadKeepsTheBytesQueuedForTheNextUpload) {
  VulkanRenderDevice device;
  ParameterSlotKey   key{NodeId{"grade.primary"}, AdjustmentInstanceId{"exposure"}};
  ExposureModel      model;
  device.Workspace().Parameters().BindSlot(key, 4, ExposureFieldBindings());
  ASSERT_TRUE(UploadPackedModel(device, key, model));

  model.SetValue(0.75F);
  device.Workspace().Device().FailNextUpload();
  WritePackedOwnerBytes(device.Workspace().Parameters(), key, model);
  EXPECT_THROW(device.Workspace().Parameters().UploadDirty(device.CommandContext()),
               std::runtime_error);
  EXPECT_TRUE(device.Workspace().Parameters().HasPendingUpload());

  device.Workspace().Parameters().UploadDirty(device.CommandContext());
  EXPECT_FALSE(device.Workspace().Parameters().HasPendingUpload());
  float value = 0.0F;
  device.Workspace().Parameters().Download(0, std::as_writable_bytes(std::span<float>(&value, 1)),
                                           device.CommandContext());
  EXPECT_FLOAT_EQ(value, 0.75F);
}

TEST_F(VulkanWorkspaceFixture, SecondCommandContextCannotRecordWhileTheFirstRecords) {
  VulkanBackend        backend;
  VulkanCommandContext first;
  VulkanCommandContext second;
  auto                 buffer = backend.CreateBuffer(64);
  backend.UploadBufferRange(buffer, 0, std::vector<std::byte>(64, std::byte{2}), first);
  EXPECT_THROW(
      backend.UploadBufferRange(buffer, 0, std::vector<std::byte>(64, std::byte{3}), second),
      std::logic_error);
  EXPECT_FALSE(second.IsRecording());
  backend.Wait(first);

  // After the first recording is submitted, the second context records.
  backend.UploadBufferRange(buffer, 0, std::vector<std::byte>(64, std::byte{4}), second);
  std::vector<std::byte> readback(64);
  backend.DownloadBufferRange(buffer, 0, readback, second);
  EXPECT_EQ(readback, std::vector<std::byte>(64, std::byte{4}));
}

TEST_F(VulkanWorkspaceFixture, AiMaskEdgeReportsThatItsVulkanKernelsAreNotBuilt) {
  VulkanRenderDevice device;
  try {
    device.Workspace().Device().RecordAiMaskEdge({}, {}, device.CommandContext());
    FAIL() << "RecordAiMaskEdge did not throw";
  } catch (const std::logic_error& error) {
    EXPECT_NE(std::string(error.what()).find("VK11"), std::string::npos) << error.what();
  }
  EXPECT_FALSE(device.CommandContext().IsRecording());
}

}  // namespace
}  // namespace alcedo
