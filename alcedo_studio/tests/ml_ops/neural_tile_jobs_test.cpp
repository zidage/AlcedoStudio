//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Backend-neutral Neural student tile planner tests (no CUDA link required).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <vector>

#include <opencv2/core.hpp>

#include "decoders/processor/cuda_tile_jobs.hpp"
#include "decoders/processor/neural_tile_jobs.hpp"
#include "decoders/processor/nn/demosaicnet_specs.hpp"

namespace alcedo {
namespace {

using detail::BuildTileJobs;
using detail::MakeBayerStudentTilePolicy;
using detail::MakeLegacyTilePolicy;
using detail::MakeXTransStudentTilePolicy;
using detail::NeuralTileJob;
using detail::StudentPeriodSafeStep;

// Reconstruct the historical Legacy BuildTileJobs geometry for regression.
auto LegacyJobsReference(const cv::Rect& active_rect, const cv::Size& full_size,
                         const int inner_size, const int halo) -> std::vector<NeuralTileJob> {
  std::vector<NeuralTileJob> jobs;
  for (int y = 0; y < active_rect.height; y += inner_size) {
    const int inner_h = std::min(inner_size, active_rect.height - y);
    for (int x = 0; x < active_rect.width; x += inner_size) {
      const int      inner_w = std::min(inner_size, active_rect.width - x);
      const cv::Rect inner_abs(active_rect.x + x, active_rect.y + y, inner_w, inner_h);
      const int      src_x = std::max(0, inner_abs.x - halo);
      const int      src_y = std::max(0, inner_abs.y - halo);
      const int      src_r = std::min(full_size.width, inner_abs.x + inner_abs.width + halo);
      const int      src_b = std::min(full_size.height, inner_abs.y + inner_abs.height + halo);
      const cv::Rect source_rect(src_x, src_y, src_r - src_x, src_b - src_y);
      NeuralTileJob  job;
      job.source_rect        = source_rect;
      job.inner_rect_in_tile = cv::Rect(inner_abs.x - source_rect.x, inner_abs.y - source_rect.y,
                                        inner_abs.width, inner_abs.height);
      job.output_rect        = cv::Rect(x, y, inner_abs.width, inner_abs.height);
      jobs.push_back(job);
    }
  }
  return jobs;
}

}  // namespace

// Purpose: legacy overload still matches historical RCD clamp geometry.
TEST(NeuralTileJobsTest, LegacyOverloadMatchesHistoricalGeometry) {
  const cv::Rect active(16, 16, 2500, 1800);
  const cv::Size full(3000, 2000);
  constexpr int  kInner = 1024;
  constexpr int  kHalo  = 16;

  const auto via_policy = BuildTileJobs(active, full, kInner, kHalo);
  const auto reference  = LegacyJobsReference(active, full, kInner, kHalo);
  ASSERT_EQ(via_policy.size(), reference.size());
  for (std::size_t i = 0; i < via_policy.size(); ++i) {
    EXPECT_EQ(via_policy[i].source_rect, reference[i].source_rect) << i;
    EXPECT_EQ(via_policy[i].inner_rect_in_tile, reference[i].inner_rect_in_tile) << i;
    EXPECT_EQ(via_policy[i].output_rect, reference[i].output_rect) << i;
  }
}

// Purpose: Bayer student policy uses pad32/border31 and CFA-period origins.
TEST(NeuralTileJobsTest, BayerStudentUsesPad32AndPeriodAlignedOrigins) {
  const auto policy = MakeBayerStudentTilePolicy();
  EXPECT_EQ(policy.virtual_pad.x, 32);
  EXPECT_EQ(policy.output_border.x, 31);
  EXPECT_EQ(policy.step.width, 1024);
  EXPECT_EQ(policy.output_tile.width, 1024);
  EXPECT_EQ(policy.input_tile.width, DemosaicNetBayerSpec::kTileInput);
  EXPECT_EQ(policy.cfa_period, 2);

  const cv::Rect active(0, 0, 2048, 2048);
  const auto     jobs = BuildTileJobs(active, active.size(), policy);
  ASSERT_FALSE(jobs.empty());
  for (const auto& job : jobs) {
    EXPECT_EQ(job.input_origin.x % 2, 0);
    EXPECT_EQ(job.input_origin.y % 2, 0);
  }
  EXPECT_EQ(jobs.front().input_origin, cv::Point(-32, -32));
  EXPECT_EQ(jobs.front().destination_roi.x, 0);
  EXPECT_EQ(jobs.front().destination_roi.y, 0);
}

// Purpose: X-Trans student policy uses step 1020 and period-6 origins.
TEST(NeuralTileJobsTest, XTransStudentUsesStep1020AndPeriodAlignedOrigins) {
  const auto policy = MakeXTransStudentTilePolicy();
  EXPECT_EQ(policy.step.width, 1020);
  EXPECT_EQ(policy.virtual_pad.x, 12);
  EXPECT_EQ(policy.output_tile.width, 1024);
  EXPECT_EQ(policy.input_tile.width, DemosaicNetXTransSpec::kTileInput);
  EXPECT_EQ(policy.cfa_period, 6);

  const cv::Rect active(0, 0, 3060, 2040);
  const auto     jobs = BuildTileJobs(active, active.size(), policy);
  ASSERT_FALSE(jobs.empty());
  for (const auto& job : jobs) {
    EXPECT_EQ(job.input_origin.x % 6, 0);
    EXPECT_EQ(job.input_origin.y % 6, 0);
  }
  EXPECT_EQ(jobs.front().input_origin, cv::Point(-12, -12));
}

// Purpose: X-Trans first-writer overlap discards leading (1024-1020)=4 columns on later tiles.
TEST(NeuralTileJobsTest, XTransStudentAssignsOverlapToFirstWriter) {
  const auto     policy = MakeXTransStudentTilePolicy();
  const cv::Rect active(0, 0, 2040, 1020);
  const auto     jobs = BuildTileJobs(active, active.size(), policy);

  ASSERT_GE(jobs.size(), 2u);

  bool found_overlap_owner = false;
  for (const auto& job : jobs) {
    if (job.input_origin.x == -12 + 1020) {
      EXPECT_EQ(job.model_output_roi.x, 4);
      found_overlap_owner = true;
      break;
    }
  }
  EXPECT_TRUE(found_overlap_owner);
}

// Purpose: destination ROIs cover every active pixel exactly once.
TEST(NeuralTileJobsTest, StudentDestinationRoisCoverEveryPixelExactlyOnce) {
  for (const auto& policy : {MakeBayerStudentTilePolicy(), MakeXTransStudentTilePolicy()}) {
    const int      cover_w = policy.step.width * 2 + policy.step.width / 2;
    const int      cover_h = policy.step.height + 17;
    const cv::Rect active(0, 0, cover_w, cover_h);
    const auto     jobs = BuildTileJobs(active, active.size(), policy);

    std::vector<int> hits(static_cast<std::size_t>(cover_w) * static_cast<std::size_t>(cover_h), 0);
    for (const auto& job : jobs) {
      const auto& d = job.destination_roi;
      ASSERT_EQ(d.width, job.model_output_roi.width);
      ASSERT_EQ(d.height, job.model_output_roi.height);
      for (int y = d.y; y < d.y + d.height; ++y) {
        for (int x = d.x; x < d.x + d.width; ++x) {
          hits[static_cast<std::size_t>(y) * cover_w + x] += 1;
        }
      }
    }
    for (int v : hits) {
      EXPECT_EQ(v, 1);
    }
  }
}

// Purpose: odd (non period-aligned) virtual pad is rejected before job generation.
TEST(NeuralTileJobsTest, StudentPoliciesRejectOddPad) {
  auto policy          = MakeBayerStudentTilePolicy();
  policy.virtual_pad.x = 31;
  policy.virtual_pad.y = 31;
  EXPECT_THROW(BuildTileJobs(cv::Rect(0, 0, 1024, 1024), cv::Size(1024, 1024), policy),
               std::runtime_error);
}

// Purpose: fixed product tiles pay full 1024×1024 owned geometry on every job.
TEST(NeuralTileJobsTest, FixedStudentTilesPayFullOutputTileOnEdges) {
  const auto    policy  = MakeBayerStudentTilePolicy();
  constexpr int kCoverW = 1024 * 2 + 300;
  constexpr int kCoverH = 1024 + 200;
  const auto    jobs =
      BuildTileJobs(cv::Rect(0, 0, kCoverW, kCoverH), cv::Size(kCoverW, kCoverH), policy);
  ASSERT_FALSE(jobs.empty());
  for (const auto& job : jobs) {
    EXPECT_EQ(job.owned_w, 1024);
    EXPECT_EQ(job.owned_h, 1024);
    EXPECT_EQ(job.input_w, 1086);
    EXPECT_EQ(job.input_h, 1086);
  }
}

// Purpose: specs-based OutputHeight/Width match fixed 1024 product export.
TEST(NeuralTileJobsTest, SpecProductOutputSizeMatches1024OwnedEdge) {
  EXPECT_EQ(DemosaicNetBayerSpec::OutputHeight(DemosaicNetBayerSpec::kTileInput,
                                               DemosaicNetBayerSpec::kTileInput),
            DemosaicNetBayerSpec::kTileOutput);
  EXPECT_EQ(DemosaicNetXTransSpec::OutputHeight(DemosaicNetXTransSpec::kTileInput,
                                                DemosaicNetXTransSpec::kTileInput),
            DemosaicNetXTransSpec::kTileOutput);
  EXPECT_EQ(DemosaicNetBayerSpec::OutputHeight(64, 64),
            64 - DemosaicNetBayerSpec::kNaturalSpatialLoss);
  EXPECT_EQ(DemosaicNetXTransSpec::OutputHeight(48, 48),
            48 - DemosaicNetXTransSpec::kNaturalSpatialLoss);
}

// Purpose: StudentPeriodSafeStep floors owned edge to a positive CFA-period multiple.
TEST(NeuralTileJobsTest, StudentPeriodSafeStepFloorsToCfaPeriod) {
  EXPECT_EQ(StudentPeriodSafeStep(1024, 2), 1024);
  EXPECT_EQ(StudentPeriodSafeStep(1024, 6), 1020);
  EXPECT_THROW(StudentPeriodSafeStep(0, 2), std::runtime_error);
  EXPECT_THROW(StudentPeriodSafeStep(5, 6), std::runtime_error);
}

// Purpose: cuda_tile_jobs.hpp aliases resolve to the same Neural planner types/lists.
TEST(NeuralTileJobsTest, CudaAliasTileListMatchesNeuralPlannerExactly) {
  static_assert(std::is_same_v<detail::CudaTilePolicy, detail::NeuralTilePolicy>);
  static_assert(std::is_same_v<detail::CudaTileJob, detail::NeuralTileJob>);

  for (const auto& policy : {MakeBayerStudentTilePolicy(), MakeXTransStudentTilePolicy()}) {
    const int      cover_w = policy.step.width * 2 + 137;
    const int      cover_h = policy.step.height + 53;
    const cv::Rect active(0, 0, cover_w, cover_h);
    const auto     jobs = BuildTileJobs(active, active.size(), policy);
    // Alias path is the same function; re-run via CudaTilePolicy cast to prove identity.
    const detail::CudaTilePolicy& cuda_policy = policy;
    const auto via_alias = BuildTileJobs(active, active.size(), cuda_policy);
    ASSERT_EQ(jobs.size(), via_alias.size());
    for (std::size_t i = 0; i < jobs.size(); ++i) {
      EXPECT_EQ(jobs[i].input_origin, via_alias[i].input_origin) << i;
      EXPECT_EQ(jobs[i].model_output_roi, via_alias[i].model_output_roi) << i;
      EXPECT_EQ(jobs[i].destination_roi, via_alias[i].destination_roi) << i;
      EXPECT_EQ(jobs[i].owned_w, via_alias[i].owned_w) << i;
      EXPECT_EQ(jobs[i].owned_h, via_alias[i].owned_h) << i;
      EXPECT_EQ(jobs[i].input_w, via_alias[i].input_w) << i;
      EXPECT_EQ(jobs[i].input_h, via_alias[i].input_h) << i;
    }
  }
}

}  // namespace alcedo
