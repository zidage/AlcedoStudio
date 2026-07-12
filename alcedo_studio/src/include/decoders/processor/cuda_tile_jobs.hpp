//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <stdexcept>
#include <vector>

#include <opencv2/core.hpp>

#include "decoders/processor/nn/demosaicnet_bayer.hpp"
#include "decoders/processor/nn/demosaicnet_xtrans.hpp"

namespace alcedo::detail {

// Shared CUDA tile planner for Legacy demosaic and Neural student assembly.
// One scheduler; model-specific geometry is expressed as CudaTilePolicy.

struct CudaTilePolicy {
  cv::Size  input_tile;      // student: 1086² / 1048²; legacy: variable via halo
  cv::Size  output_tile;     // 1024² for student; = step for legacy non-overlap
  cv::Size  step;            // 1024² / 1020² student; = output_tile for legacy
  cv::Point virtual_pad;     // 32,32 / 12,12 student; halo for legacy clamp path
  cv::Point output_border;   // 31,31 / 12,12 student; 0 for legacy inner ownership
  int       cfa_period = 1;  // 2 / 6 student; 1 for legacy (no phase assert)

  // When true, use the historical Legacy builder: clamp source rects to the
  // full image, no virtual signed origins, no overlap ownership discard.
  bool legacy_clamped = false;
};

// Generalized job: signed origin + disjoint owned model/destination ROIs.
// Legacy fields remain for existing RCD / interim Neural loops.
struct CudaTileJob {
  // Generalized (student) coordinates in the aligned CFA lattice.
  cv::Point input_origin;     // signed; model input top-left (may be negative)
  cv::Rect  model_output_roi; // owned sub-rect of the fixed output_tile
  cv::Rect  destination_roi;  // same extent, assembled aligned RGB

  // Legacy / interim product fields (clamped source window + inner placement).
  cv::Rect source_rect;
  cv::Rect inner_rect_in_tile;
  cv::Rect output_rect;
};

[[nodiscard]] inline auto MakeLegacyTilePolicy(const int inner_size, const int halo)
    -> CudaTilePolicy {
  if (inner_size <= 0 || halo < 0) {
    throw std::runtime_error("MakeLegacyTilePolicy: invalid tile dimensions");
  }
  CudaTilePolicy p;
  p.input_tile     = {inner_size + 2 * halo, inner_size + 2 * halo};
  p.output_tile    = {inner_size, inner_size};
  p.step           = {inner_size, inner_size};
  p.virtual_pad    = {halo, halo};
  p.output_border  = {0, 0};
  p.cfa_period     = 1;
  p.legacy_clamped = true;
  return p;
}

[[nodiscard]] inline auto MakeBayerStudentTilePolicy() -> CudaTilePolicy {
  CudaTilePolicy p;
  p.input_tile     = {BayerDemosaicNet::kTileInput, BayerDemosaicNet::kTileInput};
  p.output_tile    = {BayerDemosaicNet::kTileOutput, BayerDemosaicNet::kTileOutput};
  p.step           = {BayerDemosaicNet::kTileStep, BayerDemosaicNet::kTileStep};
  p.virtual_pad    = {BayerDemosaicNet::kTilePad, BayerDemosaicNet::kTilePad};
  p.output_border  = {BayerDemosaicNet::kTileBorder, BayerDemosaicNet::kTileBorder};
  p.cfa_period     = BayerDemosaicNet::kCfaPeriod;
  p.legacy_clamped = false;
  return p;
}

[[nodiscard]] inline auto MakeXTransStudentTilePolicy() -> CudaTilePolicy {
  CudaTilePolicy p;
  p.input_tile     = {XTransDemosaicNet::kTileInput, XTransDemosaicNet::kTileInput};
  p.output_tile    = {XTransDemosaicNet::kTileOutput, XTransDemosaicNet::kTileOutput};
  p.step           = {XTransDemosaicNet::kTileStep, XTransDemosaicNet::kTileStep};
  p.virtual_pad    = {XTransDemosaicNet::kTilePad, XTransDemosaicNet::kTilePad};
  p.output_border  = {XTransDemosaicNet::kTileBorder, XTransDemosaicNet::kTileBorder};
  p.cfa_period     = XTransDemosaicNet::kCfaPeriod;
  p.legacy_clamped = false;
  return p;
}

// Policy-driven planner. For legacy_clamped, `active_rect` is the region to cover
// in full-image coordinates and `full_size` clamps source rects. For student
// policies, `active_rect` is the aligned CFA extent to cover (typically the full
// aligned frame) and virtual pad is applied as signed origins.
[[nodiscard]] inline auto BuildTileJobs(const cv::Rect& active_rect, const cv::Size& full_size,
                                        const CudaTilePolicy& policy) -> std::vector<CudaTileJob> {
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

  std::vector<CudaTileJob> jobs;

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
        CudaTileJob    job;
        job.source_rect        = source_rect;
        job.inner_rect_in_tile = cv::Rect(inner_abs.x - source_rect.x, inner_abs.y - source_rect.y,
                                          inner_abs.width, inner_abs.height);
        job.output_rect        = cv::Rect(x, y, inner_abs.width, inner_abs.height);
        // Generalized fields mirror legacy placement for dual-use callers.
        job.input_origin       = {source_rect.x, source_rect.y};
        job.model_output_roi   = job.inner_rect_in_tile;
        job.destination_roi    = job.output_rect;
        jobs.push_back(job);
      }
    }
    return jobs;
  }

  // Student / virtual-pad path: cover active_rect with fixed-shape tiles.
  // Grid origins in the active/aligned lattice: g * step.
  // input_origin = g * step - virtual_pad  (signed, period-aligned).
  // model_output_origin in assembled coords = g * step - virtual_pad + output_border
  //   = g * step + (output_border - virtual_pad).
  // For Bayer: border-pad = 31-32 = -1 → first output origin clips one pixel.
  // For X-Trans: border-pad = 0 → first output origin at 0.
  //
  // Overlap ownership: later tiles discard leading (output_tile - step) pixels.

  const int lead_x = policy.output_tile.width - policy.step.width;
  const int lead_y = policy.output_tile.height - policy.step.height;
  if (lead_x < 0 || lead_y < 0) {
    throw std::runtime_error("BuildTileJobs: output_tile must be >= step");
  }

  // Destination coverage is the active_rect size (aligned RGB assembly size).
  const int cover_w = active_rect.width;
  const int cover_h = active_rect.height;

  // Grid until each axis's model-output origin reaches cover. Bayer pad32/border31
  // places the first origin at -1, so a pure step-based loop (gx*step < cover)
  // would leave the last row/column uncovered.
  for (int gy = 0;; ++gy) {
    const int model_out_y =
        gy * policy.step.height - policy.virtual_pad.y + policy.output_border.y;
    if (model_out_y >= cover_h) {
      break;
    }
    for (int gx = 0;; ++gx) {
      const int model_out_x =
          gx * policy.step.width - policy.virtual_pad.x + policy.output_border.x;
      if (model_out_x >= cover_w) {
        break;
      }

      const cv::Point input_origin(active_rect.x + gx * policy.step.width - policy.virtual_pad.x,
                                   active_rect.y + gy * policy.step.height - policy.virtual_pad.y);

      if ((input_origin.x % policy.cfa_period) != 0 || (input_origin.y % policy.cfa_period) != 0) {
        throw std::runtime_error(
            "BuildTileJobs: tile input origin is not period-aligned (CFA phase unsafe)");
      }

      // Owned ROI inside the fixed output_tile (first-writer: discard lead overlap).
      const int own_x0 = (gx == 0) ? 0 : lead_x;
      const int own_y0 = (gy == 0) ? 0 : lead_y;
      const int own_x1 = policy.output_tile.width;
      const int own_y1 = policy.output_tile.height;

      // Clip owned block against the destination coverage in assembled coords.
      int dst_x0 = model_out_x + own_x0;
      int dst_y0 = model_out_y + own_y0;
      int dst_x1 = model_out_x + own_x1;
      int dst_y1 = model_out_y + own_y1;

      // Intersect with [0, cover_w) × [0, cover_h) in active-relative space.
      dst_x0 = std::max(dst_x0, 0);
      dst_y0 = std::max(dst_y0, 0);
      dst_x1 = std::min(dst_x1, cover_w);
      dst_y1 = std::min(dst_y1, cover_h);

      // Re-express model_output_roi after destination clip.
      const int roi_x0 = dst_x0 - model_out_x;
      const int roi_y0 = dst_y0 - model_out_y;
      const int roi_x1 = dst_x1 - model_out_x;
      const int roi_y1 = dst_y1 - model_out_y;

      if (roi_x1 <= roi_x0 || roi_y1 <= roi_y0) {
        // Entirely outside coverage (can happen near edges with negative origins).
        continue;
      }

      CudaTileJob job;
      job.input_origin     = input_origin;
      job.model_output_roi = cv::Rect(roi_x0, roi_y0, roi_x1 - roi_x0, roi_y1 - roi_y0);
      job.destination_roi  = cv::Rect(dst_x0, dst_y0, dst_x1 - dst_x0, dst_y1 - dst_y0);

      // Interim legacy-shaped fields: fixed input window size, owned ROI as inner.
      job.source_rect =
          cv::Rect(input_origin.x, input_origin.y, policy.input_tile.width, policy.input_tile.height);
      job.inner_rect_in_tile = job.model_output_roi;
      job.output_rect        = job.destination_roi;
      jobs.push_back(job);
    }
  }

  if (jobs.empty()) {
    throw std::runtime_error("BuildTileJobs: produced no tiles");
  }

  // Coverage + disjoint ownership proof (dense integer grid).
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
    -> std::vector<CudaTileJob> {
  return BuildTileJobs(active_rect, full_size, MakeLegacyTilePolicy(inner_size, halo));
}

}  // namespace alcedo::detail
