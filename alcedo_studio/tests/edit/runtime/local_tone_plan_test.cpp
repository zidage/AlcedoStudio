//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/local_tone_executor.hpp"
#include "edit/runtime/local_tone_plan.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/geometry/source_geometry.hpp"
#include "edit/runtime/gpu_node_pass_stats.hpp"

namespace alcedo {
namespace {

auto MakeFullGeometry(std::uint32_t extent) -> ResolvedRenderGeometry {
  return ResolveRenderGeometry(MakeSourceGeometry({extent, extent}, {extent, extent}), {}, {}, {},
                               {});
}

auto MakeRoiGeometry(std::uint32_t extent) -> ResolvedRenderGeometry {
  ViewRequest view;
  view.visible_rect_in_edit_space = {0.25f, 0.0f, 0.5f, 1.0f};
  return ResolveRenderGeometry(MakeSourceGeometry({extent, extent}, {extent, extent}), {}, view, {},
                               {});
}

TEST(LocalTonePlan, FullFrameMissRebuildsSourceAndResultAndPersists) {
  LocalToneCanonicalLookup lookup;
  const auto geometry = MakeFullGeometry(64);
  const auto decision =
      DecideLocalTone(true, lookup, CoversFullEditSpace(geometry), 64, 64, 64, geometry);
  const auto trace = MakeLocalToneDecisionTrace(decision);
  EXPECT_EQ(trace.action, LocalToneAction::RebuildSourceAndResult);
  EXPECT_TRUE(trace.write_canonical_reference);
  EXPECT_FALSE(trace.reuse_source);
  EXPECT_TRUE(trace.persist_canonical);
  EXPECT_GT(trace.pyramid_level_count, 1);
}

TEST(LocalTonePlan, ValidCanonicalResultSamplesWithoutRebuild) {
  LocalToneCanonicalLookup lookup;
  lookup.source_valid     = true;
  lookup.result_valid     = true;
  lookup.source_long_edge = 64;
  lookup.extent           = {32, 32};
  const auto geometry     = MakeFullGeometry(64);
  const auto decision =
      DecideLocalTone(true, lookup, CoversFullEditSpace(geometry), 64, 64, 64, geometry);
  EXPECT_EQ(decision.action, LocalToneAction::SampleCanonical);
  EXPECT_FALSE(decision.reuse_source);
  EXPECT_TRUE(decision.persist_canonical);
}

TEST(LocalTonePlan, SliderEditReusesSourceAndRebuildsResult) {
  LocalToneCanonicalLookup lookup;
  lookup.source_valid     = true;
  lookup.result_valid     = false;
  lookup.source_long_edge = 64;
  lookup.extent           = {32, 32};
  const auto geometry     = MakeFullGeometry(64);
  const auto decision =
      DecideLocalTone(true, lookup, CoversFullEditSpace(geometry), 64, 64, 64, geometry);
  EXPECT_EQ(decision.action, LocalToneAction::RebuildResult);
  EXPECT_TRUE(decision.reuse_source);
  EXPECT_TRUE(decision.persist_canonical);
}

TEST(LocalTonePlan, ViewportWithValidCanonicalSamples) {
  LocalToneCanonicalLookup lookup;
  lookup.source_valid     = true;
  lookup.result_valid     = true;
  lookup.source_long_edge = 64;
  lookup.extent           = {32, 32};
  const auto geometry     = MakeRoiGeometry(64);
  ASSERT_FALSE(CoversFullEditSpace(geometry));
  const auto decision =
      DecideLocalTone(true, lookup, false, 32, 32, 64, geometry);
  EXPECT_EQ(decision.action, LocalToneAction::SampleCanonical);
}

TEST(LocalTonePlan, IsolatedViewportWithoutCacheDoesNotPersist) {
  LocalToneCanonicalLookup lookup;
  const auto geometry = MakeRoiGeometry(64);
  const auto decision = DecideLocalTone(true, lookup, false, 32, 32, 64, geometry);
  EXPECT_EQ(decision.action, LocalToneAction::RebuildSourceAndResult);
  EXPECT_FALSE(decision.write_canonical_reference);
  EXPECT_FALSE(decision.persist_canonical);
}

TEST(LocalTonePlan, QualityBaseBypassIgnoresCacheAndDoesNotPersist) {
  LocalToneCanonicalLookup lookup;
  lookup.source_valid     = true;
  lookup.result_valid     = true;
  lookup.source_long_edge = 64;
  const auto geometry     = MakeFullGeometry(64);
  const auto decision =
      DecideLocalTone(false, lookup, true, 64, 64, 64, geometry);
  EXPECT_EQ(decision.action, LocalToneAction::RebuildSourceAndResult);
  EXPECT_TRUE(decision.write_canonical_reference);
  EXPECT_FALSE(decision.persist_canonical);
}

TEST(LocalTonePlan, EquivalentLookupsProduceIdenticalTraces) {
  LocalToneCanonicalLookup lookup;
  lookup.source_valid     = true;
  lookup.result_valid     = false;
  lookup.source_long_edge = 128;
  lookup.extent           = {64, 64};
  const auto geometry     = MakeFullGeometry(128);
  const auto first =
      MakeLocalToneDecisionTrace(DecideLocalTone(true, lookup, true, 128, 128, 128, geometry));
  const auto second =
      MakeLocalToneDecisionTrace(DecideLocalTone(true, lookup, true, 128, 128, 128, geometry));
  EXPECT_EQ(first.action, second.action);
  EXPECT_EQ(first.persist_canonical, second.persist_canonical);
  EXPECT_EQ(first.reuse_source, second.reuse_source);
  EXPECT_EQ(first.pyramid_level_count, second.pyramid_level_count);
  EXPECT_EQ(first.mask_extent.width, second.mask_extent.width);
}

struct FakeToneTexture {
  std::uint32_t width  = 16;
  std::uint32_t height = 16;
};

struct FakeToneWorkspace {
  bool rendering = true;
  bool persist   = true;

  [[nodiscard]] auto IsRendering() const -> bool { return rendering; }
  [[nodiscard]] auto PersistsResult(const GraphValueId&) const -> bool { return persist; }
};

struct FakeToneDevice {
  FakeToneWorkspace workspace;
  GpuNodePassStats  stats;

  auto Workspace() -> FakeToneWorkspace& { return workspace; }
  auto PassStats() -> GpuNodePassStats& { return stats; }
};

struct FakeToneOps {
  using Device       = FakeToneDevice;
  using Texture      = FakeToneTexture;
  using ScratchPlane = int;

  static constexpr const char* kErrorPrefix = "FakeLocalTone";

  static inline std::vector<std::string> log;
  static inline bool                     throw_on_remap = false;
  static inline LocalToneCanonicalLookup lookup;

  static auto TextureWidth(const Texture& texture) -> std::uint32_t { return texture.width; }
  static auto TextureHeight(const Texture& texture) -> std::uint32_t { return texture.height; }
  static auto TransientBytes(FakeToneDevice&) -> std::size_t { return log.size(); }
  static auto LookupCanonical(FakeToneDevice&, const GraphValueId&, const GraphValueId&, int,
                              const ResolvedRenderGeometry&) -> LocalToneCanonicalLookup {
    return lookup;
  }
  static void ApplyCanonicalSample(FakeToneDevice&, const Texture&, Texture&, const GraphValueId&,
                                   const GraphValueId&, const LocalToneDecision&, std::uint32_t,
                                   std::uint32_t) {
    log.emplace_back("sample");
  }
  static auto CanonicalResourceId(FakeToneDevice&, const GraphValueId&) -> std::uint64_t {
    return 7;
  }
  static auto BindCanonicalSourcePlane(FakeToneDevice&, const GraphValueId&, std::size_t) -> int {
    log.emplace_back("bind-source");
    return 1;
  }
  static auto AllocateScratchPlane(FakeToneDevice&, std::size_t) -> int {
    log.emplace_back("scratch");
    return 2;
  }
  static void ExtractReference(FakeToneDevice&, const Texture&, int, std::uint32_t, std::uint32_t,
                               const LocalToneDecision&, const ResolvedRenderGeometry&) {
    log.emplace_back("extract-reference");
  }
  static void Extract(FakeToneDevice&, const Texture&, int, std::uint32_t, std::uint32_t,
                      const LocalToneDecision&) {
    log.emplace_back("extract");
  }
  static void PyramidDown(FakeToneDevice&, int, int, const LocalToneDecision&, int) {
    log.emplace_back("pyr-down");
  }
  static void FillZero(FakeToneDevice&, int) { log.emplace_back("fill"); }
  static void Remap(FakeToneDevice&, int, int, const LocalToneDecision&,
                    const local_tone_mapping::LlfSample&, float) {
    log.emplace_back("remap");
    if (throw_on_remap) {
      throw std::runtime_error("FakeLocalTone: injected remap failure");
    }
  }
  static void Select(FakeToneDevice&, int, int, int, int, int, int, const LocalToneDecision&, int,
                     const local_tone_mapping::LlfSample&, const local_tone_mapping::LlfSample&,
                     bool, bool, bool) {
    log.emplace_back("select");
  }
  static void Collapse(FakeToneDevice&, int, int, int, const LocalToneDecision&, int) {
    log.emplace_back("collapse");
  }
  static void ApplyAdjusted(FakeToneDevice&, const Texture&, Texture&, int, int, std::uint32_t,
                            std::uint32_t, const LocalToneDecision&) {
    log.emplace_back("apply");
  }
  static void PersistCanonicalSource(FakeToneDevice&, int, const GraphValueId&,
                                     const LocalToneDecision&, int) {
    log.emplace_back("persist-source");
  }
  static void PersistCanonicalResult(FakeToneDevice&, int, const GraphValueId&,
                                     const LocalToneDecision&, int) {
    log.emplace_back("persist-result");
  }
};

TEST(LocalToneExecutor, RemapFailureDoesNotPersistCanonicalPlanes) {
  FakeToneOps::log.clear();
  FakeToneOps::throw_on_remap = true;
  FakeToneOps::lookup         = {};
  FakeToneDevice device;
  FakeToneTexture input;
  FakeToneTexture output;
  EXPECT_THROW((void)LocalToneExecutor<FakeToneOps>::Execute(device, input, output, NodeId{"grade"},
                                                             40.0f, 0.0f, MakeFullGeometry(16)),
               std::runtime_error);
  bool persisted = false;
  for (const auto& entry : FakeToneOps::log) {
    persisted = persisted || entry == "persist-source" || entry == "persist-result";
  }
  EXPECT_FALSE(persisted);
  FakeToneOps::throw_on_remap = false;
}

TEST(LocalToneExecutor, SliderEditBindsCanonicalSourceAndPersistsResultOnly) {
  FakeToneOps::log.clear();
  FakeToneOps::throw_on_remap          = false;
  FakeToneOps::lookup.source_valid     = true;
  FakeToneOps::lookup.result_valid     = false;
  FakeToneOps::lookup.source_long_edge = 16;
  FakeToneOps::lookup.extent           = {8, 8};
  FakeToneDevice device;
  FakeToneTexture input;
  FakeToneTexture output;
  const auto result = LocalToneExecutor<FakeToneOps>::Execute(
      device, input, output, NodeId{"grade"}, 70.0f, 0.0f, MakeFullGeometry(16));
  EXPECT_TRUE(result.rebuilt_reference);
  EXPECT_FALSE(result.sampled_canonical_reference);
  bool bound_source   = false;
  bool extracted      = false;
  bool persist_source = false;
  bool persist_result = false;
  for (const auto& entry : FakeToneOps::log) {
    bound_source   = bound_source || entry == "bind-source";
    extracted      = extracted || entry == "extract" || entry == "extract-reference";
    persist_source = persist_source || entry == "persist-source";
    persist_result = persist_result || entry == "persist-result";
  }
  EXPECT_TRUE(bound_source);
  EXPECT_FALSE(extracted);
  EXPECT_FALSE(persist_source);
  EXPECT_TRUE(persist_result);
}

TEST(LocalToneExecutor, ValidCanonicalLookupSamplesWithoutExtract) {
  FakeToneOps::log.clear();
  FakeToneOps::throw_on_remap         = false;
  FakeToneOps::lookup.source_valid    = true;
  FakeToneOps::lookup.result_valid    = true;
  FakeToneOps::lookup.source_long_edge = 16;
  FakeToneOps::lookup.extent          = {8, 8};
  FakeToneDevice device;
  FakeToneTexture input;
  FakeToneTexture output;
  const auto result = LocalToneExecutor<FakeToneOps>::Execute(
      device, input, output, NodeId{"grade"}, 25.0f, 0.0f, MakeFullGeometry(16));
  EXPECT_TRUE(result.sampled_canonical_reference);
  EXPECT_FALSE(result.rebuilt_reference);
  ASSERT_EQ(FakeToneOps::log.size(), 1U);
  EXPECT_EQ(FakeToneOps::log.front(), "sample");
}

}  // namespace
}  // namespace alcedo
