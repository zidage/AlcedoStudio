//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>

#include "decoders/processor/cuda_tile_jobs.hpp"

namespace alcedo {
namespace {

using detail::BuildTileJobs;
using detail::CudaTileJob;
using detail::MakeBayerStudentTilePolicy;
using detail::MakeLegacyTilePolicy;
using detail::MakeXTransStudentTilePolicy;

// Reconstruct the historical Legacy BuildTileJobs geometry for regression.
auto LegacyJobsReference(const cv::Rect& active_rect, const cv::Size& full_size,
                         const int inner_size, const int halo) -> std::vector<CudaTileJob> {
  std::vector<CudaTileJob> jobs;
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
      CudaTileJob    job;
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

TEST(CudaTileJobsTest, LegacyOverloadMatchesHistoricalGeometry) {
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

TEST(CudaTileJobsTest, BuildTileJobs_BayerStudentUsesPad32AndPeriodAlignedOrigins) {
  const auto policy = MakeBayerStudentTilePolicy();
  EXPECT_EQ(policy.virtual_pad.x, 32);
  EXPECT_EQ(policy.output_border.x, 31);
  EXPECT_EQ(policy.step.width, 1024);
  EXPECT_EQ(policy.cfa_period, 2);

  const cv::Rect active(0, 0, 2048, 2048);
  const auto     jobs = BuildTileJobs(active, active.size(), policy);
  ASSERT_FALSE(jobs.empty());
  for (const auto& job : jobs) {
    EXPECT_EQ(job.input_origin.x % 2, 0);
    EXPECT_EQ(job.input_origin.y % 2, 0);
  }
  // First tile origin = -pad.
  EXPECT_EQ(jobs.front().input_origin, cv::Point(-32, -32));
  // First model output origin in assembled coords is -1 (clip one pixel).
  EXPECT_EQ(jobs.front().destination_roi.x, 0);
  EXPECT_EQ(jobs.front().destination_roi.y, 0);
}

TEST(CudaTileJobsTest, BuildTileJobs_XTransStudentUsesStep1020AndPeriodAlignedOrigins) {
  const auto policy = MakeXTransStudentTilePolicy();
  EXPECT_EQ(policy.step.width, 1020);
  EXPECT_EQ(policy.virtual_pad.x, 12);
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

TEST(CudaTileJobsTest, BuildTileJobs_XTransStudentAssignsOverlapToFirstWriter) {
  const auto     policy = MakeXTransStudentTilePolicy();
  const cv::Rect active(0, 0, 2040, 1020);
  const auto     jobs = BuildTileJobs(active, active.size(), policy);

  // Find two horizontally adjacent tiles on the first row.
  std::vector<const CudaTileJob*> row0;
  for (const auto& job : jobs) {
    if (job.destination_roi.y == 0 ||
        (job.destination_roi.y < 4 && job.input_origin.y == -12)) {
      row0.push_back(&job);
    }
  }
  // At least two tiles along X for this coverage.
  ASSERT_GE(jobs.size(), 2u);

  // Second tile in X discards 4 leading columns (1024 - 1020).
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

TEST(CudaTileJobsTest, BuildTileJobs_StudentDestinationRoisCoverEveryPixelExactlyOnce) {
  for (const auto& policy : {MakeBayerStudentTilePolicy(), MakeXTransStudentTilePolicy()}) {
    // Cover a non-multiple of step to force edge clips.
    const int        cover_w = policy.step.width * 2 + policy.step.width / 2;
    const int        cover_h = policy.step.height + 17;
    const cv::Rect   active(0, 0, cover_w, cover_h);
    const auto       jobs = BuildTileJobs(active, active.size(), policy);

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

TEST(CudaTileJobsTest, StudentPoliciesRejectOddPad) {
  auto policy           = MakeBayerStudentTilePolicy();
  policy.virtual_pad.x  = 31;  // forbidden odd pad
  policy.virtual_pad.y  = 31;
  EXPECT_THROW(BuildTileJobs(cv::Rect(0, 0, 1024, 1024), cv::Size(1024, 1024), policy),
               std::runtime_error);
}

// Purpose: product owned-output edges (incl. experimental 512) preserve pad/border
// context and period-safe steps (Bayer step=owned; X-Trans step=floor(owned/6)*6).
TEST(CudaTileJobsTest, StudentProductTilePolicyFormulasForValidatedEdges) {
  using detail::kStudentProductTileEdges;
  using detail::MakeBayerStudentTilePolicy;
  using detail::MakeXTransStudentTilePolicy;
  using detail::StudentPeriodSafeStep;

  for (const int owned : kStudentProductTileEdges) {
    const auto bayer = MakeBayerStudentTilePolicy(owned);
    EXPECT_EQ(bayer.output_tile.width, owned) << owned;
    EXPECT_EQ(bayer.input_tile.width, owned + 2 * BayerDemosaicNet::kTileBorder) << owned;
    EXPECT_EQ(bayer.virtual_pad.x, BayerDemosaicNet::kTilePad);
    EXPECT_EQ(bayer.output_border.x, BayerDemosaicNet::kTileBorder);
    EXPECT_EQ(bayer.step.width, owned) << owned;
    EXPECT_EQ(bayer.cfa_period, 2);
    // Exact 1K fallback constants.
    if (owned == 1024) {
      EXPECT_EQ(bayer.input_tile.width, BayerDemosaicNet::kTileInput);
      EXPECT_EQ(bayer.step.width, BayerDemosaicNet::kTileStep);
    }

    const auto xtrans = MakeXTransStudentTilePolicy(owned);
    EXPECT_EQ(xtrans.output_tile.width, owned) << owned;
    EXPECT_EQ(xtrans.input_tile.width, owned + 2 * XTransDemosaicNet::kTileBorder) << owned;
    EXPECT_EQ(xtrans.virtual_pad.x, XTransDemosaicNet::kTilePad);
    EXPECT_EQ(xtrans.output_border.x, XTransDemosaicNet::kTileBorder);
    EXPECT_EQ(xtrans.step.width, StudentPeriodSafeStep(owned, 6)) << owned;
    EXPECT_EQ(xtrans.step.width % 6, 0) << owned;
    if (owned == 1024) {
      EXPECT_EQ(xtrans.input_tile.width, XTransDemosaicNet::kTileInput);
      EXPECT_EQ(xtrans.step.width, XTransDemosaicNet::kTileStep);
      EXPECT_EQ(xtrans.step.width, 1020);
    }
  }
}

// Purpose: every generated job origin is CFA-period aligned for all product edges.
TEST(CudaTileJobsTest, StudentProductTilesHaveCfaPeriodAlignedOrigins) {
  using detail::kStudentProductTileEdges;
  for (const int owned : kStudentProductTileEdges) {
    for (const auto& policy :
         {MakeBayerStudentTilePolicy(owned), MakeXTransStudentTilePolicy(owned)}) {
      const int      cover_w = policy.step.width * 2 + policy.step.width / 3 + 17;
      const int      cover_h = policy.step.height + 41;
      const cv::Rect active(0, 0, cover_w, cover_h);
      const auto     jobs = BuildTileJobs(active, active.size(), policy);
      ASSERT_FALSE(jobs.empty()) << owned;
      for (const auto& job : jobs) {
        EXPECT_EQ(job.input_origin.x % policy.cfa_period, 0) << owned;
        EXPECT_EQ(job.input_origin.y % policy.cfa_period, 0) << owned;
      }
    }
  }
}

// Purpose: destination ROIs cover every active pixel exactly once for larger edges.
TEST(CudaTileJobsTest, StudentProductTilesDestinationRoisCoverExactlyOnce) {
  using detail::kStudentProductTileEdges;
  for (const int owned : {1024, 1536, 2048}) {
    for (const auto& policy :
         {MakeBayerStudentTilePolicy(owned), MakeXTransStudentTilePolicy(owned)}) {
      const int      cover_w = policy.step.width * 2 + policy.step.width / 2;
      const int      cover_h = policy.step.height + 17;
      const cv::Rect active(0, 0, cover_w, cover_h);
      const auto     jobs = BuildTileJobs(active, active.size(), policy);

      std::vector<int> hits(static_cast<std::size_t>(cover_w) * static_cast<std::size_t>(cover_h),
                            0);
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
        EXPECT_EQ(v, 1) << "owned=" << owned;
      }
    }
  }
}

// Purpose: low free/total VRAM forces deterministic fallback to the 1K control policy.
TEST(CudaTileJobsTest, StudentProductTileSelectionFallsBackTo1KWhenVramTight) {
  using detail::EstimateStudentTileOwnedBytes;
  using detail::SelectStudentProductTileEdge;

  // Tiny budgets → only 1K can possibly pass (or even 1K may fail budget; still returns 1K).
  EXPECT_EQ(SelectStudentProductTileEdge(/*is_bayer=*/true, /*free=*/1, /*total=*/1), 1024);
  EXPECT_EQ(SelectStudentProductTileEdge(/*is_bayer=*/false, /*free=*/1, /*total=*/1), 1024);

  // Generous budgets accept the largest *retained* edge (P2: only 1024 retained).
  constexpr std::size_t kHuge = 16ull * 1024ull * 1024ull * 1024ull;  // 16 GiB
  EXPECT_EQ(SelectStudentProductTileEdge(true, kHuge, kHuge), 1024);
  EXPECT_EQ(SelectStudentProductTileEdge(false, kHuge, kHuge), 1024);
  // Force still reaches experimental larger edges for harness re-measurement.
  EXPECT_EQ(SelectStudentProductTileEdge(true, kHuge, kHuge, /*force_edge=*/3072), 3072);

  // Budget that admits 1024 but not 1536 for Bayer.
  const std::size_t bayer_1k = EstimateStudentTileOwnedBytes(true, 1024);
  // total/free such that 35% total and 50% free both exceed 1K but not 1536.
  // free_budget = 0.5 * free, total_budget = 0.35 * total.
  // Need free_budget >= bayer_1k and free_budget < bayer_1536.
  const std::size_t bayer_1536 = EstimateStudentTileOwnedBytes(true, 1536);
  ASSERT_GT(bayer_1536, bayer_1k);
  const std::size_t free  = static_cast<std::size_t>(static_cast<double>(bayer_1k + 1024) / 0.50);
  const std::size_t total = static_cast<std::size_t>(static_cast<double>(bayer_1k + 1024) / 0.35);
  EXPECT_EQ(SelectStudentProductTileEdge(true, free, total), 1024);

  // Force edge ignores budgets (experimental sizes allowed).
  EXPECT_EQ(SelectStudentProductTileEdge(true, 1, 1, /*force_edge=*/2048), 2048);
  EXPECT_EQ(SelectStudentProductTileEdge(true, 1, 1, /*force_edge=*/512), 512);
  EXPECT_THROW(SelectStudentProductTileEdge(true, kHuge, kHuge, /*force_edge=*/128),
               std::runtime_error);
}

// Purpose: product OutputHeight/Width export-crop for larger square tiles; free-size natural.
TEST(CudaTileJobsTest, StudentProductOutputSizeMatchesOwnedEdge) {
  for (const int owned : detail::kStudentProductTileEdges) {
    const int bayer_in = owned + 2 * BayerDemosaicNet::kTileBorder;
    EXPECT_EQ(BayerDemosaicNet::OutputHeight(bayer_in, bayer_in), owned) << owned;
    EXPECT_EQ(BayerDemosaicNet::OutputWidth(bayer_in, bayer_in), owned) << owned;

    const int xtrans_in = owned + 2 * XTransDemosaicNet::kTileBorder;
    EXPECT_EQ(XTransDemosaicNet::OutputHeight(xtrans_in, xtrans_in), owned) << owned;
    EXPECT_EQ(XTransDemosaicNet::OutputWidth(xtrans_in, xtrans_in), owned) << owned;
  }
  // Free-size patch stays natural (no product export).
  EXPECT_EQ(BayerDemosaicNet::OutputHeight(64, 64), 64 - BayerDemosaicNet::kNaturalSpatialLoss);
  EXPECT_EQ(XTransDemosaicNet::OutputHeight(48, 48), 48 - XTransDemosaicNet::kNaturalSpatialLoss);
}

}  // namespace alcedo
