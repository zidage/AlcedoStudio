//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include <opencv2/core.hpp>

#include "decoders/processor/nn/demosaicnet_bayer.hpp"
#include "decoders/processor/nn/demosaicnet_xtrans.hpp"

namespace alcedo::detail {

// Shared CUDA tile planner for Legacy demosaic and Neural student assembly.
// One scheduler; model-specific geometry is expressed as CudaTilePolicy.

struct CudaTilePolicy {
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

// Measured / experimental owned-export edges. Harness force-override may use any
// value the policy constructors accept (period-aligned, >= kStudentMinOwnedTileEdge).
inline constexpr std::array<int, 5> kStudentProductTileEdges = {512, 1024, 1536, 2048, 3072};

// Smallest owned edge allowed for product-style virtual-pad tiling (export context
// still uses fixed pad/border; not a product default).
inline constexpr int kStudentMinOwnedTileEdge = 256;

// Product auto-select walks only retained edges (passed full-frame latency gates).
// P2: 1536/2048/3072 regress. Smaller edges are experimental until gated.
inline constexpr std::array<int, 1> kStudentProductRetainedTileEdges = {1024};

// Conservative VRAM budgets for auto-selecting the largest retained policy.
inline constexpr double kStudentTileMaxTotalVramFraction = 0.35;
inline constexpr double kStudentTileMaxFreeVramFraction  = 0.50;

// Optional harness/test override for the product owned edge (0 = auto-select).
// ProcessCudaTiled and SelectStudentProductTileEdge honor this when non-zero.
[[nodiscard]] inline int& StudentProductTileEdgeOverride() {
  // Function-local static keeps the override process-wide without a .cpp TU.
  static int override_edge = 0;
  return override_edge;
}

inline void SetStudentProductTileEdgeOverride(const int owned_output_edge) {
  StudentProductTileEdgeOverride() = owned_output_edge;
}

[[nodiscard]] inline auto GetStudentProductTileEdgeOverride() -> int {
  return StudentProductTileEdgeOverride();
}

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

// Derive student tile geometry from an explicit owned-output edge while retaining
// the fixed export context of the 1K policies (Bayer pad32/border31, X-Trans
// pad12/border12). Do not infer pad/border from kNaturalSpatialLoss alone.
[[nodiscard]] inline auto MakeBayerStudentTilePolicy(const int owned_output_edge) -> CudaTilePolicy {
  if (owned_output_edge < kStudentMinOwnedTileEdge) {
    throw std::runtime_error("MakeBayerStudentTilePolicy: owned edge below minimum product tile");
  }
  if ((owned_output_edge % BayerDemosaicNet::kCfaPeriod) != 0) {
    throw std::runtime_error("MakeBayerStudentTilePolicy: owned edge must be CFA-period aligned");
  }
  // Keep the 1K control constants exact: input = owned + 2*31, pad=32, border=31.
  const int input =
      owned_output_edge + 2 * BayerDemosaicNet::kTileBorder;  // 1024 → 1086
  const int step = StudentPeriodSafeStep(owned_output_edge, BayerDemosaicNet::kCfaPeriod);
  CudaTilePolicy p;
  p.input_tile     = {input, input};
  p.output_tile    = {owned_output_edge, owned_output_edge};
  p.step           = {step, step};
  p.virtual_pad    = {BayerDemosaicNet::kTilePad, BayerDemosaicNet::kTilePad};
  p.output_border  = {BayerDemosaicNet::kTileBorder, BayerDemosaicNet::kTileBorder};
  p.cfa_period     = BayerDemosaicNet::kCfaPeriod;
  p.legacy_clamped = false;
  return p;
}

[[nodiscard]] inline auto MakeXTransStudentTilePolicy(const int owned_output_edge)
    -> CudaTilePolicy {
  if (owned_output_edge < kStudentMinOwnedTileEdge) {
    throw std::runtime_error("MakeXTransStudentTilePolicy: owned edge below minimum product tile");
  }
  // Keep the 1K control constants exact: input = owned + 2*12, pad=12, border=12.
  // Step is floored to a multiple of 6 (1024 → 1020).
  const int input =
      owned_output_edge + 2 * XTransDemosaicNet::kTileBorder;  // 1024 → 1048
  const int step = StudentPeriodSafeStep(owned_output_edge, XTransDemosaicNet::kCfaPeriod);
  CudaTilePolicy p;
  p.input_tile     = {input, input};
  p.output_tile    = {owned_output_edge, owned_output_edge};
  p.step           = {step, step};
  p.virtual_pad    = {XTransDemosaicNet::kTilePad, XTransDemosaicNet::kTilePad};
  p.output_border  = {XTransDemosaicNet::kTileBorder, XTransDemosaicNet::kTileBorder};
  p.cfa_period     = XTransDemosaicNet::kCfaPeriod;
  p.legacy_clamped = false;
  return p;
}

// Best-effort estimate of NeuralDemosaicWorkspace::OwnedDeviceBytes for one tile
// (activation peak-live + NCHW input/output + HWC RGB tile). Used for VRAM selection.
[[nodiscard]] inline auto EstimateStudentTileOwnedBytes(const bool is_bayer,
                                                        const int owned_output_edge)
    -> std::size_t {
  const CudaTilePolicy policy =
      is_bayer ? MakeBayerStudentTilePolicy(owned_output_edge)
               : MakeXTransStudentTilePolicy(owned_output_edge);
  const int hin = policy.input_tile.width;
  const int win = policy.input_tile.height;
  const int oh  = policy.output_tile.width;
  const int ow  = policy.output_tile.height;
  const std::size_t activation =
      is_bayer ? BayerDemosaicNet::EstimateWorkspaceBytes(hin, win, 1)
               : XTransDemosaicNet::EstimateWorkspaceBytes(hin, win, 1);
  const std::size_t input_bytes =
      static_cast<std::size_t>(3) * static_cast<std::size_t>(hin) * static_cast<std::size_t>(win) *
      sizeof(float);
  const std::size_t output_bytes =
      static_cast<std::size_t>(3) * static_cast<std::size_t>(oh) * static_cast<std::size_t>(ow) *
      sizeof(float);
  // HWC RGB tile: assume contiguous CV_32FC3 (no row padding in the estimate).
  const std::size_t rgb_bytes = output_bytes;
  return activation + input_bytes + output_bytes + rgb_bytes;
}

// Largest retained product edge whose reserved owned memory fits the budget.
// force_edge / harness override may request any experimental edge (>= min, period-safe).
// Allocation failure at product warm-up must still fall back to the next smaller retained edge.
[[nodiscard]] inline auto SelectStudentProductTileEdge(const bool is_bayer,
                                                       const std::size_t free_vram_bytes,
                                                       const std::size_t total_vram_bytes,
                                                       const int force_edge = 0) -> int {
  const int override_edge = GetStudentProductTileEdgeOverride();
  const int requested     = force_edge > 0 ? force_edge : override_edge;

  if (requested > 0) {
    // Force/override: experimental sizes (512, 768, …) as well as the P2 matrix.
    if (requested < kStudentMinOwnedTileEdge) {
      throw std::runtime_error("SelectStudentProductTileEdge: force/override edge below minimum");
    }
    if (is_bayer && (requested % BayerDemosaicNet::kCfaPeriod) != 0) {
      throw std::runtime_error("SelectStudentProductTileEdge: Bayer force edge must be even");
    }
    return requested;
  }

  const std::size_t total_budget =
      static_cast<std::size_t>(static_cast<double>(total_vram_bytes) *
                               kStudentTileMaxTotalVramFraction);
  const std::size_t free_budget =
      static_cast<std::size_t>(static_cast<double>(free_vram_bytes) *
                               kStudentTileMaxFreeVramFraction);

  // Prefer largest *retained* edges first (not the full experimental candidate list).
  for (auto it = kStudentProductRetainedTileEdges.rbegin();
       it != kStudentProductRetainedTileEdges.rend(); ++it) {
    const int         edge  = *it;
    const std::size_t owned = EstimateStudentTileOwnedBytes(is_bayer, edge);
    if (owned <= total_budget && owned <= free_budget) {
      return edge;
    }
  }
  // Always fall back to the 1K control policy.
  return kStudentProductRetainedTileEdges.front();
}

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

// Default product policy: retained 1024 owned-output control.
[[nodiscard]] inline auto MakeBayerStudentTilePolicy() -> CudaTilePolicy {
  return MakeBayerStudentTilePolicy(BayerDemosaicNet::kTileOutput);
}

[[nodiscard]] inline auto MakeXTransStudentTilePolicy() -> CudaTilePolicy {
  return MakeXTransStudentTilePolicy(XTransDemosaicNet::kTileOutput);
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
