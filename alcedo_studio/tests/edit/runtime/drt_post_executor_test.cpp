//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/drt_post_executor.hpp"
#include "edit/runtime/drt_post_schedule.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/neighbor_executor.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "edit/graph/graph_ids.hpp"
#include "edit/runtime/adjustment_runtime.hpp"

namespace alcedo {
namespace {

auto Enabled(AdjustmentBehavior behavior) -> GradeNeighborParams {
  GradeNeighborParams params;
  params.behavior = static_cast<std::uint32_t>(behavior);
  params.enabled  = 1;
  return params;
}

auto Disabled() -> GradeNeighborParams {
  GradeNeighborParams params;
  params.enabled = 0;
  return params;
}

TEST(DrtPostSchedule, DisabledNeighborhoodsCopySceneThenDisplay) {
  const GradeNeighborParams order[] = {Disabled(), Disabled()};
  const auto schedule = MakeDrtPostSchedule(order);
  const auto trace    = MakeDrtPostDecisionTrace(schedule);
  EXPECT_TRUE(trace.copy_scene_to_post);
  EXPECT_EQ(trace.enabled_neighborhood_count, 0U);
  EXPECT_TRUE(trace.display_transform_scheduled);
  const auto none = DrtWriteSequence(0);
  ASSERT_EQ(none.size(), 1U);
  EXPECT_EQ(none.front(), DrtWriteTarget::Display);
}

TEST(DrtPostSchedule, EnabledNeighborhoodsKeepCompilerOrderAndPingPongDestinations) {
  const GradeNeighborParams order[] = {Disabled(), Enabled(AdjustmentBehavior::Sharpen),
                                       Enabled(AdjustmentBehavior::Clarity),
                                       Enabled(AdjustmentBehavior::FilmGrain)};
  const auto schedule = MakeDrtPostSchedule(order);
  const auto trace    = MakeDrtPostDecisionTrace(schedule);
  EXPECT_FALSE(trace.copy_scene_to_post);
  ASSERT_EQ(trace.enabled_neighborhood_count, 3U);
  EXPECT_EQ(static_cast<AdjustmentBehavior>(schedule.enabled[0].behavior),
            AdjustmentBehavior::Sharpen);
  EXPECT_EQ(static_cast<AdjustmentBehavior>(schedule.enabled[1].behavior),
            AdjustmentBehavior::Clarity);
  const auto dests = DrtWriteSequence(3);
  ASSERT_EQ(dests.size(), 4U);
  EXPECT_EQ(dests[0], DrtWriteTarget::FreeWorkMember);
  EXPECT_EQ(dests[1], DrtWriteTarget::Display);
  EXPECT_EQ(dests[2], DrtWriteTarget::FreeWorkMember);
  EXPECT_EQ(dests[3], DrtWriteTarget::Display);
}

struct FakeNeighborTexture {};

struct FakeHorizontalScratch {
  std::vector<std::string>* log = nullptr;

  FakeHorizontalScratch() = default;
  explicit FakeHorizontalScratch(std::vector<std::string>* owned_log) : log(owned_log) {}
  FakeHorizontalScratch(const FakeHorizontalScratch&)            = delete;
  auto operator=(const FakeHorizontalScratch&) -> FakeHorizontalScratch& = delete;
  FakeHorizontalScratch(FakeHorizontalScratch&& other) noexcept : log(other.log) {
    other.log = nullptr;
  }
  auto operator=(FakeHorizontalScratch&& other) noexcept -> FakeHorizontalScratch& {
    log       = other.log;
    other.log = nullptr;
    return *this;
  }
  ~FakeHorizontalScratch() {
    if (log != nullptr) {
      log->emplace_back("release-h-scratch");
    }
  }
};

struct FakeNeighborDevice {};

struct FakeNeighborOps {
  using Device            = FakeNeighborDevice;
  using HorizontalScratch = FakeHorizontalScratch;
  using LutBinding        = int;

  static inline std::vector<std::string> log;
  static inline bool                     throw_on_horizontal = false;

  static auto AcquireHorizontalScratch(FakeNeighborDevice&, std::uint32_t, std::uint32_t)
      -> HorizontalScratch {
    log.emplace_back("acquire-h-scratch");
    return FakeHorizontalScratch{&log};
  }

  static void DispatchHorizontal(FakeNeighborDevice&, const FrameSceneBinding&, HorizontalScratch&,
                                 const NeighborWork&, std::uint32_t, std::uint32_t) {
    log.emplace_back("horizontal");
    if (throw_on_horizontal) {
      throw std::runtime_error("FakeNeighbor: injected horizontal failure");
    }
  }

  static void DispatchVerticalApply(FakeNeighborDevice&, const FrameSceneBinding&,
                                    HorizontalScratch&, const FrameSceneBinding&,
                                    const FrameSceneBinding&, const LutBinding&, const NeighborWork&,
                                    float, const GraphValueId*, std::uint32_t, std::uint32_t) {
    log.emplace_back("vertical");
  }
};

TEST(NeighborExecutor, AcquiresScratchThenStartsHorizontalThenVerticalThenReleases) {
  FakeNeighborOps::log.clear();
  FakeNeighborOps::throw_on_horizontal = false;
  FakeNeighborDevice device;
  NeighborWork work;
  const auto src = FrameSceneBinding::WorkImage(SceneWorkMember::Member0);
  const auto dst = FrameSceneBinding::WorkImage(SceneWorkMember::Member1);
  NeighborExecutor<FakeNeighborOps>::Execute(device, src, dst, src, 0, work, 1.0f, nullptr, 8, 8);
  ASSERT_EQ(FakeNeighborOps::log.size(), 4U);
  EXPECT_EQ(FakeNeighborOps::log[0], "acquire-h-scratch");
  EXPECT_EQ(FakeNeighborOps::log[1], "horizontal");
  EXPECT_EQ(FakeNeighborOps::log[2], "vertical");
  EXPECT_EQ(FakeNeighborOps::log[3], "release-h-scratch");
}

TEST(NeighborExecutor, HorizontalFailureReleasesScratchWithoutVerticalStart) {
  FakeNeighborOps::log.clear();
  FakeNeighborOps::throw_on_horizontal = true;
  FakeNeighborDevice device;
  NeighborWork work;
  const auto src = FrameSceneBinding::WorkImage(SceneWorkMember::Member0);
  const auto dst = FrameSceneBinding::WorkImage(SceneWorkMember::Member1);
  EXPECT_THROW((NeighborExecutor<FakeNeighborOps>::Execute(device, src, dst, src, 0, work, 1.0f,
                                                           nullptr, 8, 8)),
               std::runtime_error);
  ASSERT_EQ(FakeNeighborOps::log.size(), 3U);
  EXPECT_EQ(FakeNeighborOps::log[0], "acquire-h-scratch");
  EXPECT_EQ(FakeNeighborOps::log[1], "horizontal");
  EXPECT_EQ(FakeNeighborOps::log[2], "release-h-scratch");
  FakeNeighborOps::throw_on_horizontal = false;
}

TEST(DrtWriteSequence, ZeroPostsWriteDisplayOnly) {
  const auto sequence = DrtWriteSequence(0);
  ASSERT_EQ(sequence.size(), 1U);
  EXPECT_EQ(sequence.front(), DrtWriteTarget::Display);
}

TEST(DrtWriteSequence, OnePostUsesFreeMemberThenDisplay) {
  const auto sequence = DrtWriteSequence(1);
  ASSERT_EQ(sequence.size(), 2U);
  EXPECT_EQ(sequence[0], DrtWriteTarget::FreeWorkMember);
  EXPECT_EQ(sequence[1], DrtWriteTarget::Display);
}

TEST(DrtWriteSequence, TwoPostsStartAndEndOnDisplay) {
  const auto sequence = DrtWriteSequence(2);
  ASSERT_EQ(sequence.size(), 3U);
  EXPECT_EQ(sequence[0], DrtWriteTarget::Display);
  EXPECT_EQ(sequence[1], DrtWriteTarget::FreeWorkMember);
  EXPECT_EQ(sequence[2], DrtWriteTarget::Display);
}

}  // namespace
}  // namespace alcedo
