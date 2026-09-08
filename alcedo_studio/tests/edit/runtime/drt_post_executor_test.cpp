//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/drt_post_executor.hpp"
#include "edit/runtime/drt_post_schedule.hpp"
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
  EXPECT_TRUE(DrtNeighborhoodDestinations(NodeId{"drt"},
                                          GraphValueId{NodeId{"drt"}, PortId{"display"}},
                                          0)
                  .empty());
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
  const GraphValueId display_output{NodeId{"drt"}, PortId{"display"}};
  const auto dests = DrtNeighborhoodDestinations(NodeId{"drt"}, display_output, 3);
  ASSERT_EQ(dests.size(), 3U);
  EXPECT_EQ(dests[0], (GraphValueId{NodeId{"drt"}, PortId{"runtime.ping"}}));
  EXPECT_EQ(dests[1], (GraphValueId{NodeId{"drt"}, PortId{"runtime.pong"}}));
  EXPECT_EQ(dests[2], display_output);
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
  using Texture           = FakeNeighborTexture;
  using HorizontalScratch = FakeHorizontalScratch;
  using LutBinding        = int;

  static inline std::vector<std::string> log;
  static inline bool                     throw_on_horizontal = false;

  static auto AcquireHorizontalScratch(FakeNeighborDevice&, std::uint32_t, std::uint32_t)
      -> HorizontalScratch {
    log.emplace_back("acquire-h-scratch");
    return FakeHorizontalScratch{&log};
  }

  static auto HorizontalScratchTexture(HorizontalScratch&) -> Texture& {
    static FakeNeighborTexture texture;
    return texture;
  }

  static auto SceneTexture(FakeNeighborDevice&, const GraphValueId&) -> Texture& {
    static FakeNeighborTexture texture;
    return texture;
  }

  static void DispatchHorizontal(FakeNeighborDevice&, const Texture&, Texture&, const NeighborWork&,
                                 std::uint32_t, std::uint32_t) {
    log.emplace_back("horizontal");
    if (throw_on_horizontal) {
      throw std::runtime_error("FakeNeighbor: injected horizontal failure");
    }
  }

  static void DispatchVerticalApply(FakeNeighborDevice&, const Texture&, const Texture&, Texture&,
                                    const LutBinding&, const NeighborWork&, std::uint32_t,
                                    std::uint32_t) {
    log.emplace_back("vertical");
  }
};

TEST(NeighborExecutor, AcquiresScratchThenStartsHorizontalThenVerticalThenReleases) {
  FakeNeighborOps::log.clear();
  FakeNeighborOps::throw_on_horizontal = false;
  FakeNeighborDevice device;
  NeighborWork work;
  NeighborExecutor<FakeNeighborOps>::Execute(device, GraphValueId{NodeId{"src"}, PortId{"image"}},
                                             GraphValueId{NodeId{"dst"}, PortId{"image"}}, 0, work,
                                             8, 8);
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
  EXPECT_THROW((NeighborExecutor<FakeNeighborOps>::Execute(
                    device, GraphValueId{NodeId{"src"}, PortId{"image"}},
                    GraphValueId{NodeId{"dst"}, PortId{"image"}}, 0, work, 8, 8)),
               std::runtime_error);
  ASSERT_EQ(FakeNeighborOps::log.size(), 3U);
  EXPECT_EQ(FakeNeighborOps::log[0], "acquire-h-scratch");
  EXPECT_EQ(FakeNeighborOps::log[1], "horizontal");
  EXPECT_EQ(FakeNeighborOps::log[2], "release-h-scratch");
  FakeNeighborOps::throw_on_horizontal = false;
}

struct FakeDrtImage {
  bool empty = false;
  FakeNeighborTexture texture;
  auto Empty() const -> bool { return empty; }
  auto Texture() -> FakeNeighborTexture& { return texture; }
};

struct FakeDrtImages {
  FakeDrtImage input;
  FakeDrtImage output;
  auto Find(const GraphValueId&) -> FakeDrtImage* { return &input; }
};

struct FakeDrtWorkspace {
  FakeDrtImages                 images;
  std::vector<GraphValueId>     released;
  auto                          IsRendering() const -> bool { return true; }
  auto                          Images() -> FakeDrtImages& { return images; }
  void                          ReleaseConsumedImage(const GraphValueId& id) { released.push_back(id); }
};

struct FakeDrtDevice {
  FakeDrtWorkspace workspace;
  auto Workspace() -> FakeDrtWorkspace& { return workspace; }
};

struct FakeDrtOps {
  using Device            = FakeDrtDevice;
  using Texture           = FakeNeighborTexture;
  using HorizontalScratch = FakeHorizontalScratch;
  using LutBinding        = int;

  static constexpr const char* kErrorPrefix = "FakeDrt";
  static inline std::vector<std::string> log;

  static auto AcquireHorizontalScratch(FakeDrtDevice&, std::uint32_t, std::uint32_t)
      -> HorizontalScratch {
    log.emplace_back("acquire-h-scratch");
    return FakeHorizontalScratch{&log};
  }
  static auto HorizontalScratchTexture(HorizontalScratch&) -> Texture& {
    static FakeNeighborTexture texture;
    return texture;
  }
  static void DispatchHorizontal(FakeDrtDevice&, const Texture&, Texture&, const NeighborWork&,
                                 std::uint32_t, std::uint32_t) {
    log.emplace_back("horizontal");
  }
  static void DispatchVerticalApply(FakeDrtDevice&, const Texture&, const Texture&, Texture&,
                                    const LutBinding&, const NeighborWork&, std::uint32_t,
                                    std::uint32_t) {
    log.emplace_back("vertical");
  }
  static auto AcquireOutput(FakeDrtDevice&, const GraphValueId&, std::uint32_t, std::uint32_t)
      -> Texture& {
    log.emplace_back("acquire-output");
    static FakeNeighborTexture texture;
    return texture;
  }
  static auto SceneTexture(FakeDrtDevice&, const GraphValueId&) -> Texture& {
    static FakeNeighborTexture texture;
    return texture;
  }
  static void CopyTexture(FakeDrtDevice&, const GraphValueId&, const GraphValueId&) {
    log.emplace_back("copy");
  }
  static void DispatchDisplayTransform(FakeDrtDevice&, const Texture&, Texture&, const NodeId&,
                                       std::uint32_t, std::uint32_t) {
    log.emplace_back("display");
  }
};

TEST(DrtPostExecutor, EmptyNeighborhoodCopiesSceneWithoutKernelStarts) {
  FakeDrtOps::log.clear();
  FakeDrtDevice device;
  DrtPostSchedule schedule;
  schedule.copy_scene_to_post = true;
  const GraphValueId input{NodeId{"grade"}, PortId{"image"}};
  const GraphValueId output{NodeId{"drt"}, PortId{"display"}};
  const auto scene =
      DrtPostExecutor<FakeDrtOps>::ApplyNeighborhoods(device, schedule, {}, input, output,
                                                      NodeId{"drt"}, 0, 8, 8);
  EXPECT_EQ(scene, output);
  ASSERT_EQ(FakeDrtOps::log.size(), 2U);
  EXPECT_EQ(FakeDrtOps::log[0], "acquire-output");
  EXPECT_EQ(FakeDrtOps::log[1], "copy");
}

TEST(DrtPostExecutor, TwoEnabledNeighborhoodsUseSharedHorizontalThenVerticalOrder) {
  FakeDrtOps::log.clear();
  FakeDrtDevice device;
  DrtPostSchedule schedule;
  schedule.enabled = {Enabled(AdjustmentBehavior::Sharpen), Enabled(AdjustmentBehavior::Clarity)};
  std::vector<NeighborWork> works(2);
  works[0].params = schedule.enabled[0];
  works[1].params = schedule.enabled[1];
  const GraphValueId input{NodeId{"grade"}, PortId{"image"}};
  const GraphValueId output{NodeId{"drt"}, PortId{"display"}};
  const auto scene = DrtPostExecutor<FakeDrtOps>::ApplyNeighborhoods(
      device, schedule, works, input, output, NodeId{"drt"}, 0, 8, 8);
  EXPECT_EQ(scene, output);
  const std::vector<std::string> expected = {
      "acquire-output", "acquire-h-scratch", "horizontal", "vertical", "release-h-scratch",
      "acquire-output", "acquire-h-scratch", "horizontal", "vertical", "release-h-scratch"};
  EXPECT_EQ(FakeDrtOps::log, expected);
  ASSERT_EQ(device.workspace.released.size(), 2U);
  EXPECT_EQ(device.workspace.released[0], (GraphValueId{NodeId{"drt"}, PortId{"runtime.ping"}}));
  EXPECT_EQ(device.workspace.released[1], (GraphValueId{NodeId{"drt"}, PortId{"runtime.pong"}}));
}

TEST(DrtPostExecutor, DisplayTransformStartsBeforeNeighborhoodWrites) {
  FakeDrtOps::log.clear();
  FakeDrtDevice device;
  DrtPostSchedule schedule;
  schedule.enabled = {Enabled(AdjustmentBehavior::Sharpen)};
  std::vector<NeighborWork> works(1);
  works[0].params = schedule.enabled[0];
  const GraphValueId input{NodeId{"grade"}, PortId{"image"}};
  const GraphValueId output{NodeId{"drt"}, PortId{"runtime.display_base"}};
  const GraphValueId display{NodeId{"drt"}, PortId{"display"}};
  DrtPostExecutor<FakeDrtOps>::DispatchDisplay(device, input, output, NodeId{"drt"}, 8, 8);
  (void)DrtPostExecutor<FakeDrtOps>::ApplyNeighborhoods(
      device, schedule, works, output, display, NodeId{"drt"}, 0, 8, 8);
  ASSERT_EQ(FakeDrtOps::log.size(), 7U);
  EXPECT_EQ(FakeDrtOps::log[0], "acquire-output");
  EXPECT_EQ(FakeDrtOps::log[1], "display");
  EXPECT_EQ(FakeDrtOps::log[2], "acquire-output");
  EXPECT_EQ(FakeDrtOps::log[3], "acquire-h-scratch");
  EXPECT_EQ(FakeDrtOps::log[4], "horizontal");
  EXPECT_EQ(FakeDrtOps::log[5], "vertical");
  EXPECT_EQ(FakeDrtOps::log[6], "release-h-scratch");
}

}  // namespace
}  // namespace alcedo
