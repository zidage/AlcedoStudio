//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <opencv2/core.hpp>

#include "decoders/processor/nn/demosaicnet_specs.hpp"

namespace alcedo::detail {

// Shared student/legacy tile planner for Neural DemosaicNet and Legacy demosaic.
// Backend-neutral: geometry only; no CUDA or OpenCL dependencies.

struct NeuralTilePolicy {
  cv::Size  input_tile;      // student: owned+2*border; legacy: variable via halo
  cv::Size  output_tile;     // owned export edge for student; = step for legacy
  cv::Size  step;            // period-safe owned step; = output_tile for legacy
  cv::Point virtual_pad;     // 32,32 / 12,12 student; halo for legacy clamp path
  cv::Point output_border;   // 31,31 / 12,12 student; 0 for legacy inner ownership
  int       cfa_period = 1;  // 2 / 6 student; 1 for legacy (no phase assert)

  // When true, use the historical Legacy builder: clamp source rects to the
  // full image, no virtual signed origins, no overlap ownership discard.
  bool legacy_clamped = false;
};

// Period-safe grid step for a requested owned export edge (first-writer overlap
// when owned is not a multiple of the CFA period).
[[nodiscard]] inline auto StudentPeriodSafeStep(const int owned_output_edge, const int cfa_period)
    -> int {
  if (owned_output_edge <= 0 || cfa_period <= 0) {
    throw std::runtime_error("StudentPeriodSafeStep: invalid owned edge or period");
  }
  const int step = (owned_output_edge / cfa_period) * cfa_period;
  if (step <= 0) {
    throw std::runtime_error("StudentPeriodSafeStep: owned edge smaller than CFA period");
  }
  return step;
}

// Generalized job: signed origin + disjoint owned model/destination ROIs.
// Legacy fields remain for existing RCD / interim Neural loops.
struct NeuralTileJob {
  // Generalized (student) coordinates in the aligned CFA lattice.
  cv::Point input_origin;      // signed; model input top-left (may be negative)
  cv::Rect  model_output_roi;  // owned sub-rect of this job's output tile
  cv::Rect  destination_roi;   // same extent, assembled aligned RGB

  // Per-job student model geometry (fixed product tiles set owned = output_tile).
  // input = owned + 2*border on each axis.
  int owned_w = 0;
  int owned_h = 0;
  int input_w = 0;
  int input_h = 0;

  // Legacy / interim product fields (clamped source window + inner placement).
  cv::Rect source_rect;
  cv::Rect inner_rect_in_tile;
  cv::Rect output_rect;
};

[[nodiscard]] inline auto MakeLegacyTilePolicy(const int inner_size, const int halo)
    -> NeuralTilePolicy {
  if (inner_size <= 0 || halo < 0) {
    throw std::runtime_error("MakeLegacyTilePolicy: invalid tile dimensions");
  }
  NeuralTilePolicy p;
  p.input_tile     = {inner_size + 2 * halo, inner_size + 2 * halo};
  p.output_tile    = {inner_size, inner_size};
  p.step           = {inner_size, inner_size};
  p.virtual_pad    = {halo, halo};
  p.output_border  = {0, 0};
  p.cfa_period     = 1;
  p.legacy_clamped = true;
  return p;
}

// Product student policy: fixed 1024 owned-output square (Bayer pad32/border31).
[[nodiscard]] inline auto MakeBayerStudentTilePolicy() -> NeuralTilePolicy {
  constexpr int owned = DemosaicNetBayerSpec::kTileOutput;  // 1024
  const int input     = owned + 2 * DemosaicNetBayerSpec::kTileBorder;
  const int step      = StudentPeriodSafeStep(owned, DemosaicNetBayerSpec::kCfaPeriod);
  NeuralTilePolicy p;
  p.input_tile     = {input, input};
  p.output_tile    = {owned, owned};
  p.step           = {step, step};
  p.virtual_pad    = {DemosaicNetBayerSpec::kTilePad, DemosaicNetBayerSpec::kTilePad};
  p.output_border  = {DemosaicNetBayerSpec::kTileBorder, DemosaicNetBayerSpec::kTileBorder};
  p.cfa_period     = DemosaicNetBayerSpec::kCfaPeriod;
  p.legacy_clamped = false;
  return p;
}

// Product student policy: fixed 1024 owned-output square (X-Trans pad12/border12;
// step floored to a multiple of 6 → 1020).
[[nodiscard]] inline auto MakeXTransStudentTilePolicy() -> NeuralTilePolicy {
  constexpr int owned = DemosaicNetXTransSpec::kTileOutput;  // 1024
  const int input     = owned + 2 * DemosaicNetXTransSpec::kTileBorder;
  const int step      = StudentPeriodSafeStep(owned, DemosaicNetXTransSpec::kCfaPeriod);
  NeuralTilePolicy p;
  p.input_tile     = {input, input};
  p.output_tile    = {owned, owned};
  p.step           = {step, step};
  p.virtual_pad    = {DemosaicNetXTransSpec::kTilePad, DemosaicNetXTransSpec::kTilePad};
  p.output_border  = {DemosaicNetXTransSpec::kTileBorder, DemosaicNetXTransSpec::kTileBorder};
  p.cfa_period     = DemosaicNetXTransSpec::kCfaPeriod;
  p.legacy_clamped = false;
  return p;
}

// One axis span for student virtual-pad tiling (cover-relative coordinates).
struct StudentAxisSpan {
  int model_out = 0;  // model output origin along the axis
  int owned     = 0;  // model output size along the axis (fixed max owned)
  int dest0     = 0;  // destination interval [dest0, dest1)
  int dest1     = 0;
  int roi0      = 0;  // model_output_roi start (= dest0 - model_out after clip)
};

// Build fixed max-owned spans along one axis (product path: no ragged rebalance).
[[nodiscard]] inline auto PlanStudentAxisSpans(const int cover, const int max_owned, const int step,
                                               const int pad, const int border)
    -> std::vector<StudentAxisSpan> {
  if (cover <= 0 || max_owned <= 0 || step <= 0) {
    throw std::runtime_error("PlanStudentAxisSpans: invalid cover/owned/step");
  }
  const int lead = max_owned - step;
  if (lead < 0) {
    throw std::runtime_error("PlanStudentAxisSpans: max_owned must be >= step");
  }

  std::vector<StudentAxisSpan> spans;
  for (int g = 0;; ++g) {
    const int model_out = g * step - pad + border;
    if (model_out >= cover) {
      break;
    }
    const int own0 = (g == 0) ? 0 : lead;
    const int own1 = max_owned;

    int dst0 = model_out + own0;
    int dst1 = model_out + own1;
    dst0     = std::max(dst0, 0);
    dst1     = std::min(dst1, cover);
    if (dst1 <= dst0) {
      continue;
    }
    StudentAxisSpan s;
    s.model_out = model_out;
    s.owned     = max_owned;
    s.dest0     = dst0;
    s.dest1     = dst1;
    s.roi0      = dst0 - model_out;
    spans.push_back(s);
  }
  if (spans.empty()) {
    throw std::runtime_error("PlanStudentAxisSpans: produced no spans");
  }
  return spans;
}

inline void FillStudentJobGeometry(NeuralTileJob& job, const NeuralTilePolicy& policy,
                                   const int owned_w, const int owned_h, const int model_roi_x,
                                   const int model_roi_y, const int dest_w, const int dest_h) {
  const int border_x = policy.output_border.x;
  const int border_y = policy.output_border.y;
  job.owned_w        = owned_w;
  job.owned_h        = owned_h;
  job.input_w        = owned_w + 2 * border_x;
  job.input_h        = owned_h + 2 * border_y;
  job.model_output_roi =
      cv::Rect(model_roi_x, model_roi_y, dest_w, dest_h);
  job.inner_rect_in_tile = job.model_output_roi;
  job.source_rect =
      cv::Rect(job.input_origin.x, job.input_origin.y, job.input_w, job.input_h);
}

// Policy-driven planner. For legacy_clamped, `active_rect` is the region to cover
// in full-image coordinates and `full_size` clamps source rects. For student
// policies, `active_rect` is the aligned CFA extent to cover (typically the full
// aligned frame) and virtual pad is applied as signed origins.
[[nodiscard]] inline auto BuildTileJobs(const cv::Rect& active_rect, const cv::Size& full_size,
                                        const NeuralTilePolicy& policy)
    -> std::vector<NeuralTileJob> {
  if (active_rect.width <= 0 || active_rect.height <= 0) {
    throw std::runtime_error("BuildTileJobs: active_rect is empty");
  }
  if (policy.step.width <= 0 || policy.step.height <= 0) {
    throw std::runtime_error("BuildTileJobs: step must be positive");
  }
  if (policy.output_tile.width <= 0 || policy.output_tile.height <= 0) {
    throw std::runtime_error("BuildTileJobs: output_tile must be positive");
  }
  if (policy.cfa_period <= 0) {
    throw std::runtime_error("BuildTileJobs: cfa_period must be positive");
  }
  if ((policy.virtual_pad.x % policy.cfa_period) != 0 ||
      (policy.virtual_pad.y % policy.cfa_period) != 0) {
    throw std::runtime_error("BuildTileJobs: virtual_pad must be a multiple of cfa_period");
  }
  if ((policy.step.width % policy.cfa_period) != 0 ||
      (policy.step.height % policy.cfa_period) != 0) {
    throw std::runtime_error("BuildTileJobs: step must be a multiple of cfa_period");
  }

  std::vector<NeuralTileJob> jobs;

  if (policy.legacy_clamped) {
    const int inner_size = policy.step.width;
    const int halo       = policy.virtual_pad.x;
    if (policy.step.width != policy.step.height || policy.virtual_pad.x != policy.virtual_pad.y) {
      throw std::runtime_error("BuildTileJobs: legacy policy requires square step/halo");
    }
    for (int y = 0; y < active_rect.height; y += inner_size) {
      const int inner_h = std::min(inner_size, active_rect.height - y);
      for (int x = 0; x < active_rect.width; x += inner_size) {
        const int inner_w = std::min(inner_size, active_rect.width - x);

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
        // Generalized fields mirror legacy placement for dual-use callers.
        job.input_origin     = {source_rect.x, source_rect.y};
        job.model_output_roi = job.inner_rect_in_tile;
        job.destination_roi  = job.output_rect;
        jobs.push_back(job);
      }
    }
    return jobs;
  }

  // Student / virtual-pad path: cover active_rect with fixed max-owned tiles.
  // Grid origins in the active/aligned lattice: input_origin = model_out - border.
  // model_out = g * step - pad + border.
  // For Bayer: border-pad = 31-32 = -1 → first output origin clips one pixel.
  // For X-Trans: border-pad = 0 → first output origin at 0.
  // Overlap ownership: later tiles discard leading (output_tile - step) pixels.

  if (policy.output_tile.width < policy.step.width ||
      policy.output_tile.height < policy.step.height) {
    throw std::runtime_error("BuildTileJobs: output_tile must be >= step");
  }

  const int cover_w = active_rect.width;
  const int cover_h = active_rect.height;

  const auto x_spans = PlanStudentAxisSpans(cover_w, policy.output_tile.width, policy.step.width,
                                            policy.virtual_pad.x, policy.output_border.x);
  const auto y_spans = PlanStudentAxisSpans(cover_h, policy.output_tile.height, policy.step.height,
                                            policy.virtual_pad.y, policy.output_border.y);

  for (const auto& ys : y_spans) {
    for (const auto& xs : x_spans) {
      // input_origin = model_out - output_border (signed, period-aligned when pad/border are).
      const cv::Point input_origin(active_rect.x + xs.model_out - policy.output_border.x,
                                   active_rect.y + ys.model_out - policy.output_border.y);

      if ((input_origin.x % policy.cfa_period) != 0 || (input_origin.y % policy.cfa_period) != 0) {
        throw std::runtime_error(
            "BuildTileJobs: tile input origin is not period-aligned (CFA phase unsafe)");
      }

      const int dest_w = xs.dest1 - xs.dest0;
      const int dest_h = ys.dest1 - ys.dest0;
      if (dest_w <= 0 || dest_h <= 0) {
        continue;
      }
      if (xs.roi0 < 0 || ys.roi0 < 0 || xs.roi0 + dest_w > xs.owned ||
          ys.roi0 + dest_h > ys.owned) {
        throw std::runtime_error("BuildTileJobs: model ROI exceeds per-job owned size");
      }

      NeuralTileJob job;
      job.input_origin    = input_origin;
      job.destination_roi = cv::Rect(xs.dest0, ys.dest0, dest_w, dest_h);
      job.output_rect     = job.destination_roi;
      FillStudentJobGeometry(job, policy, xs.owned, ys.owned, xs.roi0, ys.roi0, dest_w, dest_h);
      jobs.push_back(job);
    }
  }

  if (jobs.empty()) {
    throw std::runtime_error("BuildTileJobs: produced no tiles");
  }

  // Coverage + exclusive ownership proof (dense integer grid).
  std::vector<std::uint8_t> coverage(static_cast<std::size_t>(cover_w) *
                                         static_cast<std::size_t>(cover_h),
                                     0);
  for (const auto& job : jobs) {
    const cv::Rect& d = job.destination_roi;
    if (d.x < 0 || d.y < 0 || d.x + d.width > cover_w || d.y + d.height > cover_h) {
      throw std::runtime_error("BuildTileJobs: destination ROI out of coverage bounds");
    }
    for (int y = d.y; y < d.y + d.height; ++y) {
      for (int x = d.x; x < d.x + d.width; ++x) {
        const std::size_t idx =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(cover_w) +
            static_cast<std::size_t>(x);
        if (coverage[idx] != 0) {
          throw std::runtime_error("BuildTileJobs: overlapping destination ROIs");
        }
        coverage[idx] = 1;
      }
    }
  }
  for (const auto v : coverage) {
    if (v == 0) {
      throw std::runtime_error("BuildTileJobs: uncovered destination pixels");
    }
  }

  return jobs;
}

// Legacy overload: unchanged geometry for RCD / historical Neural halo tiling.
[[nodiscard]] inline auto BuildTileJobs(const cv::Rect& active_rect, const cv::Size& full_size,
                                        const int inner_size, const int halo)
    -> std::vector<NeuralTileJob> {
  return BuildTileJobs(active_rect, full_size, MakeLegacyTilePolicy(inner_size, halo));
}

}  // namespace alcedo::detail
