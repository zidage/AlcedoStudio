//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/automation/headless_frame_sink.hpp"

#include <gtest/gtest.h>

namespace alcedo::automation {
namespace {

TEST(HeadlessFrameSinkTest, HostVisibleMappingMatchesRequestedSize) {
  HeadlessFrameSink sink;
  sink.EnsureSize(64, 32);

  const FrameWriteMapping mapping = sink.MapResourceForWrite(FrameMemoryDomain::CudaDevice);

  ASSERT_TRUE(static_cast<bool>(mapping));
  EXPECT_EQ(sink.GetWidth(), 64);
  EXPECT_EQ(sink.GetHeight(), 32);
  EXPECT_EQ(mapping.row_bytes, 64U * 4U * sizeof(float));
  EXPECT_EQ(mapping.pixel_format, FramePixelFormat::RGBA32F);
  EXPECT_EQ(mapping.memory_domain, FrameMemoryDomain::HostVisible);
  EXPECT_EQ(mapping.target_type, FrameWriteTargetType::LinearBuffer);

  // The whole frame is writable host memory.
  auto* pixels              = static_cast<float*>(mapping.data);
  pixels[(64 * 32 * 4) - 1] = 1.0F;
  sink.UnmapResource();
}

TEST(HeadlessFrameSinkTest, ResizeReplacesTheHostBuffer) {
  HeadlessFrameSink sink;
  sink.EnsureSize(8, 8);
  sink.EnsureSize(16, 4);

  const FrameWriteMapping mapping = sink.MapResourceForWrite(FrameMemoryDomain::HostVisible);

  ASSERT_TRUE(static_cast<bool>(mapping));
  EXPECT_EQ(sink.GetWidth(), 16);
  EXPECT_EQ(sink.GetHeight(), 4);
  EXPECT_EQ(mapping.row_bytes, 16U * 4U * sizeof(float));
}

TEST(HeadlessFrameSinkTest, MappingBeforeSizeIsEmpty) {
  HeadlessFrameSink sink;

  EXPECT_FALSE(static_cast<bool>(sink.MapResourceForWrite(FrameMemoryDomain::CudaDevice)));
}

TEST(HeadlessFrameSinkTest, NotifyFrameReadyCountsFrames) {
  HeadlessFrameSink sink;
  sink.EnsureSize(4, 4);

  sink.NotifyFrameReady(FrameCompletionSubmission{});
  sink.NotifyFrameReady(FrameCompletionSubmission{});

  EXPECT_EQ(sink.ready_frame_count(), 2U);
}

}  // namespace
}  // namespace alcedo::automation
