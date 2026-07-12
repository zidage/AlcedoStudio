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

}  // namespace alcedo
