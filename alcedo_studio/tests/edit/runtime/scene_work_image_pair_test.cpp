//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/scene_work_image_pair.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

struct HostSceneWorkBackend {
  class SceneWorkImage {
   public:
    SceneWorkImage() = default;
    SceneWorkImage(std::uint32_t width, std::uint32_t height, std::uint64_t id)
        : width_(width), height_(height), resource_id_(id),
          bytes_(static_cast<std::size_t>(width) * height *
                 TextureFormatBytesPerPixel(TextureFormat::Rgba32f)) {}

    SceneWorkImage(const SceneWorkImage&)                    = delete;
    auto operator=(const SceneWorkImage&) -> SceneWorkImage& = delete;
    SceneWorkImage(SceneWorkImage&&)                         = default;
    auto operator=(SceneWorkImage&&) -> SceneWorkImage&      = default;

    [[nodiscard]] auto Width() const -> std::uint32_t { return width_; }
    [[nodiscard]] auto Height() const -> std::uint32_t { return height_; }
    [[nodiscard]] auto Format() const -> TextureFormat { return TextureFormat::Rgba32f; }
    [[nodiscard]] auto Bytes() const -> std::size_t { return bytes_; }
    [[nodiscard]] auto ResourceId() const -> std::uint64_t { return resource_id_; }
    [[nodiscard]] auto Empty() const -> bool { return resource_id_ == 0; }

   private:
    std::uint32_t width_       = 0;
    std::uint32_t height_      = 0;
    std::uint64_t resource_id_ = 0;
    std::size_t   bytes_       = 0;
  };

  std::uint64_t create_count = 0;
  std::uint64_t next_id      = 1;

  auto CreateSceneWorkImage(std::uint32_t width, std::uint32_t height) -> SceneWorkImage {
    ++create_count;
    return SceneWorkImage{width, height, next_id++};
  }
};

TEST(SceneWorkImagePair, SceneWorkPairOwnsExactlyTwoRgba32fImages) {
  HostSceneWorkBackend backend;
  SceneWorkImagePair<HostSceneWorkBackend> pair;
  pair.Ensure(backend, ImageExtent{16, 9});
  EXPECT_EQ(pair.MemberCount(), 2U);
  EXPECT_EQ(backend.create_count, 2U);
  EXPECT_EQ(pair.Member(SceneWorkMember::Member0).Width(), 16U);
  EXPECT_EQ(pair.Member(SceneWorkMember::Member0).Height(), 9U);
  EXPECT_EQ(pair.Member(SceneWorkMember::Member0).Format(), TextureFormat::Rgba32f);
  EXPECT_EQ(pair.Member(SceneWorkMember::Member1).Format(), TextureFormat::Rgba32f);
  EXPECT_NE(pair.Member(SceneWorkMember::Member0).ResourceId(),
            pair.Member(SceneWorkMember::Member1).ResourceId());
}

TEST(SceneWorkImagePair, SameExtentRendersReuseSceneWorkAllocations) {
  HostSceneWorkBackend backend;
  SceneWorkImagePair<HostSceneWorkBackend> pair;
  pair.Ensure(backend, ImageExtent{8, 8});
  const auto first_ids = std::vector<std::uint64_t>{
      pair.Member(SceneWorkMember::Member0).ResourceId(),
      pair.Member(SceneWorkMember::Member1).ResourceId()};
  pair.Ensure(backend, ImageExtent{8, 8});
  pair.Ensure(backend, ImageExtent{8, 8});
  EXPECT_EQ(backend.create_count, 2U);
  EXPECT_EQ(pair.AllocationCount(), 2U);
  EXPECT_EQ(pair.Member(SceneWorkMember::Member0).ResourceId(), first_ids[0]);
  EXPECT_EQ(pair.Member(SceneWorkMember::Member1).ResourceId(), first_ids[1]);
}

TEST(SceneWorkImagePair, ExtentChangeRecreatesBothSceneWorkImagesAfterGpuCompletion) {
  HostSceneWorkBackend backend;
  SceneWorkImagePair<HostSceneWorkBackend> pair;
  pair.Ensure(backend, ImageExtent{7, 5});
  const auto first0 = pair.Member(SceneWorkMember::Member0).ResourceId();
  const auto first1 = pair.Member(SceneWorkMember::Member1).ResourceId();
  pair.Ensure(backend, ImageExtent{11, 3});
  EXPECT_EQ(backend.create_count, 4U);
  EXPECT_EQ(pair.AllocationCount(), 4U);
  EXPECT_EQ(pair.Extent().width, 11U);
  EXPECT_EQ(pair.Extent().height, 3U);
  EXPECT_NE(pair.Member(SceneWorkMember::Member0).ResourceId(), first0);
  EXPECT_NE(pair.Member(SceneWorkMember::Member1).ResourceId(), first1);
}

TEST(SceneWorkImagePair, SceneWorkBytesAreIncludedInResourceMeasurements) {
  HostSceneWorkBackend backend;
  SceneWorkImagePair<HostSceneWorkBackend> pair;
  const ImageExtent extent{32, 16};
  pair.Ensure(backend, extent);
  const auto expected = SceneWorkImagePair<HostSceneWorkBackend>::PairBytes(extent);
  EXPECT_EQ(expected, static_cast<std::size_t>(32) * 16 * 16 * 2);
  EXPECT_EQ(pair.CurrentBytes(), expected);
  EXPECT_EQ(pair.PeakBytes(), expected);
  EXPECT_EQ(pair.AllocationCount(), 2U);
  pair.Release();
  EXPECT_EQ(pair.CurrentBytes(), 0U);
  EXPECT_EQ(pair.PeakBytes(), expected);
  EXPECT_EQ(pair.MemberCount(), 0U);
}

TEST(FrameSceneBinding, DisabledAndZeroMixGradesAliasFrameInputWithoutCopy) {
  const auto cached = FrameSceneBinding::CachedImage(GraphValueId{NodeId{"develop"}, PortId{"image"}});
  const auto dest   = DestinationWorkMember(cached);
  EXPECT_EQ(dest, SceneWorkMember::Member0);
  const auto first = FrameSceneBinding::WorkImage(dest);
  EXPECT_EQ(DestinationWorkMember(first), SceneWorkMember::Member1);
  EXPECT_EQ(PeerOf(SceneWorkMember::Member1), SceneWorkMember::Member0);
  EXPECT_EQ(cached, FrameSceneBinding::CachedImage(GraphValueId{NodeId{"develop"}, PortId{"image"}}));
}

}  // namespace
}  // namespace alcedo
