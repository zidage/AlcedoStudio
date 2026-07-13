//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
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

  // P4-B: interior jobs keep output_tile; right/bottom edge jobs use rectangular
  // owned extents that match their destination ROI (workspace still sized for
  // output_tile). Default follows StudentRaggedEdgeTilesEnabled().
  bool ragged_edge_tiles = false;
};

// P4-B product/experiment toggle. false = fixed square (or uniform rect) jobs;
// true = ragged edge owned sizes. ProcessCudaTiled and BuildTileJobs honor this
// when constructing student policies (unless a policy sets ragged_edge_tiles
// explicitly after construction).
[[nodiscard]] inline auto StudentRaggedEdgeTilesEnabled() -> bool& {
  static bool enabled = false;
  return enabled;
}

inline void SetStudentRaggedEdgeTilesEnabled(const bool enabled) {
  StudentRaggedEdgeTilesEnabled() = enabled;
}

// Rebalance threshold: final owned destination extent below this is merged with
// its predecessor into two period-aligned extents (each <= max owned).
inline constexpr int kStudentRaggedMinOwnedTail = 512;

// Measured / experimental owned-export edges (square). Harness force-override may use any
// value the policy constructors accept (period-aligned, >= kStudentMinOwnedTileEdge).
inline constexpr std::array<int, 5> kStudentProductTileEdges = {512, 1024, 1536, 2048, 3072};

// P4-C experimental full-width strip owned heights (period-aligned candidates; harness).
// Exact CFA-period step is derived per policy; X-Trans floors non-multiples of 6.
inline constexpr std::array<int, 3> kStudentStripOwnedHeights = {128, 256, 512};

// Smallest owned axis for square product-style virtual-pad tiling (historical P2 floor).
// Rectangular/strip policies may use kStudentMinOwnedRectAxis (P4-C 128 strips).
inline constexpr int kStudentMinOwnedTileEdge = 256;
// Smallest owned axis for rectangular/strip product policies (P4-C).
inline constexpr int kStudentMinOwnedRectAxis = 128;

// Product auto-select walks only retained edges (passed full-frame latency gates).
// P2: 1536/2048/3072 regress. P4-C strips are experimental until gated.
inline constexpr std::array<int, 1> kStudentProductRetainedTileEdges = {1024};

// Conservative VRAM budgets for auto-selecting the largest retained policy.
inline constexpr double kStudentTileMaxTotalVramFraction = 0.35;
inline constexpr double kStudentTileMaxFreeVramFraction  = 0.50;

// Owned export shape for student product tiling (square or rectangular).
struct StudentOwnedTileShape {
  int width  = 0;  // owned export width; 0 = unset
  int height = 0;  // owned export height; 0 = unset

  [[nodiscard]] auto IsSet() const noexcept -> bool { return width > 0 && height > 0; }
  [[nodiscard]] auto IsSquare() const noexcept -> bool {
    return IsSet() && width == height;
  }
};

// Optional harness/test override for the product owned shape (0,0 = auto-select).
// ProcessCudaTiled and SelectStudentProductTileShape honor this when set.
// Square edge override (legacy) sets width=height.
[[nodiscard]] inline StudentOwnedTileShape& StudentProductTileShapeOverride() {
  static StudentOwnedTileShape override_shape{};
  return override_shape;
}

inline void SetStudentProductTileEdgeOverride(const int owned_output_edge) {
  StudentProductTileShapeOverride() =
      owned_output_edge > 0 ? StudentOwnedTileShape{owned_output_edge, owned_output_edge}
                            : StudentOwnedTileShape{};
}

inline void SetStudentProductTileShapeOverride(const int owned_w, const int owned_h) {
  if (owned_w <= 0 || owned_h <= 0) {
    StudentProductTileShapeOverride() = {};
    return;
  }
  StudentProductTileShapeOverride() = StudentOwnedTileShape{owned_w, owned_h};
}

[[nodiscard]] inline auto GetStudentProductTileEdgeOverride() -> int {
  const auto& s = StudentProductTileShapeOverride();
  // Legacy single-edge API: only report when square (rect uses GetStudentProductTileShapeOverride).
  return s.IsSquare() ? s.width : 0;
}

[[nodiscard]] inline auto GetStudentProductTileShapeOverride() -> StudentOwnedTileShape {
  return StudentProductTileShapeOverride();
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

// Derive student tile geometry from an explicit rectangular owned-output size while
// retaining the fixed export context of the 1K policies (Bayer pad32/border31,
// X-Trans pad12/border12). Do not infer pad/border from kNaturalSpatialLoss alone.
[[nodiscard]] inline auto MakeBayerStudentTilePolicy(const int owned_w, const int owned_h)
    -> CudaTilePolicy {
  if (owned_w < kStudentMinOwnedRectAxis || owned_h < kStudentMinOwnedRectAxis) {
    throw std::runtime_error("MakeBayerStudentTilePolicy: owned axis below minimum product tile");
  }
  if ((owned_w % BayerDemosaicNet::kCfaPeriod) != 0 ||
      (owned_h % BayerDemosaicNet::kCfaPeriod) != 0) {
    throw std::runtime_error("MakeBayerStudentTilePolicy: owned axes must be CFA-period aligned");
  }
  // Keep the 1K control constants exact: input = owned + 2*31, pad=32, border=31.
  const int input_w = owned_w + 2 * BayerDemosaicNet::kTileBorder;  // 1024 → 1086
  const int input_h = owned_h + 2 * BayerDemosaicNet::kTileBorder;
  const int step_w  = StudentPeriodSafeStep(owned_w, BayerDemosaicNet::kCfaPeriod);
  const int step_h  = StudentPeriodSafeStep(owned_h, BayerDemosaicNet::kCfaPeriod);
  CudaTilePolicy p;
  p.input_tile     = {input_w, input_h};
  p.output_tile    = {owned_w, owned_h};
  p.step           = {step_w, step_h};
  p.virtual_pad    = {BayerDemosaicNet::kTilePad, BayerDemosaicNet::kTilePad};
  p.output_border  = {BayerDemosaicNet::kTileBorder, BayerDemosaicNet::kTileBorder};
  p.cfa_period        = BayerDemosaicNet::kCfaPeriod;
  p.legacy_clamped    = false;
  p.ragged_edge_tiles = StudentRaggedEdgeTilesEnabled();
  return p;
}

[[nodiscard]] inline auto MakeBayerStudentTilePolicy(const int owned_output_edge) -> CudaTilePolicy {
  // Square convenience: historical P2 edge floor (256) for square product tiles.
  if (owned_output_edge < kStudentMinOwnedTileEdge) {
    throw std::runtime_error("MakeBayerStudentTilePolicy: owned edge below minimum product tile");
  }
  return MakeBayerStudentTilePolicy(owned_output_edge, owned_output_edge);
}

[[nodiscard]] inline auto MakeXTransStudentTilePolicy(const int owned_w, const int owned_h)
    -> CudaTilePolicy {
  if (owned_w < kStudentMinOwnedRectAxis || owned_h < kStudentMinOwnedRectAxis) {
    throw std::runtime_error("MakeXTransStudentTilePolicy: owned axis below minimum product tile");
  }
  // Keep the 1K control constants exact: input = owned + 2*12, pad=12, border=12.
  // Step is floored to a multiple of 6 (1024 → 1020).
  const int input_w = owned_w + 2 * XTransDemosaicNet::kTileBorder;  // 1024 → 1048
  const int input_h = owned_h + 2 * XTransDemosaicNet::kTileBorder;
  const int step_w  = StudentPeriodSafeStep(owned_w, XTransDemosaicNet::kCfaPeriod);
  const int step_h  = StudentPeriodSafeStep(owned_h, XTransDemosaicNet::kCfaPeriod);
  CudaTilePolicy p;
  p.input_tile     = {input_w, input_h};
  p.output_tile    = {owned_w, owned_h};
  p.step           = {step_w, step_h};
  p.virtual_pad    = {XTransDemosaicNet::kTilePad, XTransDemosaicNet::kTilePad};
  p.output_border  = {XTransDemosaicNet::kTileBorder, XTransDemosaicNet::kTileBorder};
  p.cfa_period        = XTransDemosaicNet::kCfaPeriod;
  p.legacy_clamped    = false;
  p.ragged_edge_tiles = StudentRaggedEdgeTilesEnabled();
  return p;
}

[[nodiscard]] inline auto MakeXTransStudentTilePolicy(const int owned_output_edge)
    -> CudaTilePolicy {
  if (owned_output_edge < kStudentMinOwnedTileEdge) {
    throw std::runtime_error("MakeXTransStudentTilePolicy: owned edge below minimum product tile");
  }
  return MakeXTransStudentTilePolicy(owned_output_edge, owned_output_edge);
}

// Full-width strip policy: one horizontal job spans `aligned_cover_w` owned pixels;
// vertical step is the strip owned height (period-safe). Cover width must be period-aligned.
[[nodiscard]] inline auto MakeBayerStudentStripPolicy(const int aligned_cover_w,
                                                      const int strip_owned_h) -> CudaTilePolicy {
  if (aligned_cover_w < kStudentMinOwnedRectAxis) {
    throw std::runtime_error("MakeBayerStudentStripPolicy: cover width below minimum");
  }
  if ((aligned_cover_w % BayerDemosaicNet::kCfaPeriod) != 0) {
    throw std::runtime_error("MakeBayerStudentStripPolicy: cover width must be CFA-period aligned");
  }
  return MakeBayerStudentTilePolicy(aligned_cover_w, strip_owned_h);
}

[[nodiscard]] inline auto MakeXTransStudentStripPolicy(const int aligned_cover_w,
                                                       const int strip_owned_h) -> CudaTilePolicy {
  if (aligned_cover_w < kStudentMinOwnedRectAxis) {
    throw std::runtime_error("MakeXTransStudentStripPolicy: cover width below minimum");
  }
  if ((aligned_cover_w % XTransDemosaicNet::kCfaPeriod) != 0) {
    throw std::runtime_error("MakeXTransStudentStripPolicy: cover width must be CFA-period aligned");
  }
  return MakeXTransStudentTilePolicy(aligned_cover_w, strip_owned_h);
}

// Best-effort estimate of NeuralDemosaicWorkspace::OwnedDeviceBytes for one tile
// (activation peak-live + NCHW input/output + HWC RGB tile). Used for VRAM selection.
[[nodiscard]] inline auto EstimateStudentTileOwnedBytes(const bool is_bayer, const int owned_w,
                                                        const int owned_h) -> std::size_t {
  const CudaTilePolicy policy = is_bayer ? MakeBayerStudentTilePolicy(owned_w, owned_h)
                                         : MakeXTransStudentTilePolicy(owned_w, owned_h);
  // input_tile is (width, height) OpenCV Size; workspace uses (H, W) spatial.
  const int hin = policy.input_tile.height;
  const int win = policy.input_tile.width;
  const int oh  = policy.output_tile.height;
  const int ow  = policy.output_tile.width;
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

[[nodiscard]] inline auto EstimateStudentTileOwnedBytes(const bool is_bayer,
                                                        const int owned_output_edge)
    -> std::size_t {
  return EstimateStudentTileOwnedBytes(is_bayer, owned_output_edge, owned_output_edge);
}

// Largest retained product square edge whose reserved owned memory fits the budget.
// force_edge / harness override may request any experimental edge (>= min, period-safe).
// Allocation failure at product warm-up must still fall back to the next smaller retained edge.
[[nodiscard]] inline auto SelectStudentProductTileEdge(const bool is_bayer,
                                                       const std::size_t free_vram_bytes,
                                                       const std::size_t total_vram_bytes,
                                                       const int force_edge = 0) -> int {
  const int override_edge = GetStudentProductTileEdgeOverride();
  const int requested     = force_edge > 0 ? force_edge : override_edge;

  if (requested > 0) {
    // Force/override: experimental square sizes (512, …) as well as the P2 matrix.
    // Rectangular / sub-256-high strips use SelectStudentProductTileShape.
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

// Select owned (w,h) for product tiling. Retained product path is square 1024.
// Harness may force rectangles / full-width strips via shape override or force_shape.
// On auto-select, only retained square edges are considered (P4-C strips not product-default
// until gated). Allocation failure falls back to 1024×1024.
[[nodiscard]] inline auto SelectStudentProductTileShape(
    const bool is_bayer, const std::size_t free_vram_bytes, const std::size_t total_vram_bytes,
    const StudentOwnedTileShape force_shape = {}) -> StudentOwnedTileShape {
  const StudentOwnedTileShape override_shape = GetStudentProductTileShapeOverride();
  const StudentOwnedTileShape requested =
      force_shape.IsSet() ? force_shape : override_shape;

  if (requested.IsSet()) {
    if (requested.width < kStudentMinOwnedRectAxis ||
        requested.height < kStudentMinOwnedRectAxis) {
      throw std::runtime_error(
          "SelectStudentProductTileShape: force/override axis below minimum");
    }
    if (is_bayer && ((requested.width % BayerDemosaicNet::kCfaPeriod) != 0 ||
                     (requested.height % BayerDemosaicNet::kCfaPeriod) != 0)) {
      throw std::runtime_error(
          "SelectStudentProductTileShape: Bayer force axes must be CFA-period aligned");
    }
    return requested;
  }

  const int edge =
      SelectStudentProductTileEdge(is_bayer, free_vram_bytes, total_vram_bytes, /*force_edge=*/0);
  return StudentOwnedTileShape{edge, edge};
}

// Generalized job: signed origin + disjoint owned model/destination ROIs.
// Legacy fields remain for existing RCD / interim Neural loops.
struct CudaTileJob {
  // Generalized (student) coordinates in the aligned CFA lattice.
  cv::Point input_origin;     // signed; model input top-left (may be negative)
  cv::Rect  model_output_roi; // owned sub-rect of this job's output tile
  cv::Rect  destination_roi;  // same extent, assembled aligned RGB

  // Per-job student model geometry (P4-B ragged edges; also filled for fixed tiles).
  // input = owned + 2*border on each axis. Workspace capacity stays at policy max.
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

// --- P4-B ragged edge helpers (axis planning + rebalance) ---

// Round required owned extent up to `align` (CFA/pack period), clamp to [min_o, max_o].
[[nodiscard]] inline auto AlignStudentOwnedExtent(const int required, const int align,
                                                  const int min_o, const int max_o) -> int {
  if (required <= 0 || align <= 0 || min_o <= 0 || max_o < min_o) {
    throw std::runtime_error("AlignStudentOwnedExtent: invalid arguments");
  }
  int o = std::max(required, min_o);
  o     = ((o + align - 1) / align) * align;
  if (o > max_o) {
    // Prefer full capacity when alignment would overshoot but capacity still covers.
    if (max_o >= required) {
      return max_o;
    }
    throw std::runtime_error("AlignStudentOwnedExtent: required exceeds max owned");
  }
  return o;
}

// One axis span for student virtual-pad tiling (cover-relative coordinates).
struct StudentAxisSpan {
  int model_out = 0;  // model output origin along the axis
  int owned     = 0;  // model output size along the axis
  int dest0     = 0;  // destination interval [dest0, dest1)
  int dest1     = 0;
  int roi0      = 0;  // model_output_roi start (= dest0 - model_out after clip)
};

// Split combined destination length W into two period-aligned positive extents ≤ max_owned.
[[nodiscard]] inline auto SplitRaggedPair(const int W, const int max_owned, const int period)
    -> std::pair<int, int> {
  if (W <= 0 || max_owned <= 0 || period <= 0) {
    throw std::runtime_error("SplitRaggedPair: invalid arguments");
  }
  if (W > 2 * max_owned) {
    throw std::runtime_error("SplitRaggedPair: combined extent exceeds two max tiles");
  }
  // Prefer a near-equal split on the period lattice.
  int Wa = (W / 2 / period) * period;
  if (Wa < period) {
    Wa = period;
  }
  if (Wa > max_owned) {
    Wa = (max_owned / period) * period;
  }
  int Wb = W - Wa;
  if (Wb > max_owned) {
    Wb = (max_owned / period) * period;
    Wa = W - Wb;
  }
  if (Wa <= 0 || Wb <= 0 || Wa > max_owned || Wb > max_owned) {
    // Last resort: put as much as possible on the left.
    Wa = std::min(W, (max_owned / period) * period);
    Wb = W - Wa;
  }
  if (Wa <= 0 || Wb <= 0 || Wa > max_owned || Wb > max_owned) {
    throw std::runtime_error("SplitRaggedPair: cannot split within max owned");
  }
  return {Wa, Wb};
}

// Build nominal (fixed max-owned) spans along one axis, then optionally ragged-shrink
// the final span and rebalance when the final destination extent is < min_tail.
[[nodiscard]] inline auto PlanStudentAxisSpans(const int cover, const int max_owned,
                                               const int step, const int pad, const int border,
                                               const int period, const bool ragged,
                                               const int min_tail, const int min_owned)
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

  if (!ragged) {
    return spans;
  }

  auto apply_owned = [&](StudentAxisSpan& s) {
    const int required = (s.dest1 - s.model_out);  // roi end = dest1 - model_out
    s.owned            = AlignStudentOwnedExtent(required, period, min_owned, max_owned);
    // Keep roi0 consistent with dest/model_out (clip already applied).
    s.roi0 = s.dest0 - s.model_out;
  };

  for (auto& s : spans) {
    apply_owned(s);
  }

  // Rebalance final tail with predecessor when the destination extent is pathologically small.
  if (spans.size() >= 2) {
    const int last_dest_w = spans.back().dest1 - spans.back().dest0;
    if (last_dest_w < min_tail) {
      const int combined0 = spans[spans.size() - 2].dest0;
      const int combined1 = spans.back().dest1;
      const int W         = combined1 - combined0;
      try {
        auto [Wa, Wb]           = SplitRaggedPair(W, max_owned, period);
        StudentAxisSpan left    = spans[spans.size() - 2];
        StudentAxisSpan right   = spans.back();

        // input_origin = model_out - border must be 0 mod period, so
        // model_out ≡ border (mod period). For the right span:
        //   lead==0 → model_out = dest0 = combined0 + Wa
        //   lead>0  → model_out = dest0 - lead
        // Adjust Wa by ±period/ residual so the right origin stays period-safe.
        auto right_model_out_for = [&](const int dest0) -> int {
          if (dest0 == 0) {
            return -pad + border;
          }
          return lead > 0 ? dest0 - lead : dest0;
        };
        auto origin_ok = [&](const int model_out) -> bool {
          int r = (model_out - border) % period;
          if (r < 0) {
            r += period;
          }
          return r == 0;
        };

        int tries = 0;
        while (tries < period && !origin_ok(right_model_out_for(combined0 + Wa))) {
          // Nudge the cut toward a period-safe lattice without exceeding max_owned.
          if (Wa + 1 <= max_owned && Wb - 1 >= 1) {
            ++Wa;
            --Wb;
          } else if (Wa - 1 >= 1 && Wb + 1 <= max_owned) {
            --Wa;
            ++Wb;
          } else {
            break;
          }
          ++tries;
        }
        // Prefer a full-period step when a single-pixel nudge is not enough (X-Trans).
        tries = 0;
        while (tries < period && !origin_ok(right_model_out_for(combined0 + Wa))) {
          if (Wa + period <= max_owned && Wb - period >= 1) {
            Wa += period;
            Wb -= period;
          } else if (Wa - period >= 1 && Wb + period <= max_owned) {
            Wa -= period;
            Wb += period;
          } else {
            break;
          }
          ++tries;
        }
        if (!origin_ok(right_model_out_for(combined0 + Wa)) || Wa <= 0 || Wb <= 0 ||
            Wa > max_owned || Wb > max_owned) {
          throw std::runtime_error("PlanStudentAxisSpans: rebalance cannot keep period alignment");
        }

        // Preserve the predecessor's model_out / first-writer lead geometry; only
        // shrink its destination. Rebuild the final span from the new cut.
        left.dest0 = combined0;
        left.dest1 = combined0 + Wa;
        left.roi0  = left.dest0 - left.model_out;
        apply_owned(left);

        right.dest0     = combined0 + Wa;
        right.dest1     = combined1;
        right.model_out = right_model_out_for(right.dest0);
        if (right.dest0 == 0) {
          right.roi0 = 0;
        } else if (lead > 0) {
          right.roi0 = lead;
        } else {
          right.roi0 = 0;
        }
        apply_owned(right);

        if (!origin_ok(right.model_out) || !origin_ok(left.model_out)) {
          throw std::runtime_error("PlanStudentAxisSpans: rebalance origin not period-aligned");
        }

        spans[spans.size() - 2] = left;
        spans.back()            = right;
      } catch (const std::runtime_error&) {
        // Keep shrink-only extents if a clean split is impossible.
      }
    }
  }

  return spans;
}

inline void FillStudentJobGeometry(CudaTileJob& job, const CudaTilePolicy& policy,
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

  // Student / virtual-pad path: cover active_rect with (optionally ragged) tiles.
  // Grid origins in the active/aligned lattice: input_origin = model_out - border.
  // model_out = g * step - pad + border (fixed path) or rebalanced cut (P4-B).
  // For Bayer: border-pad = 31-32 = -1 → first output origin clips one pixel.
  // For X-Trans: border-pad = 0 → first output origin at 0.
  // Overlap ownership: later tiles discard leading (output_tile - step) pixels.

  if (policy.output_tile.width < policy.step.width ||
      policy.output_tile.height < policy.step.height) {
    throw std::runtime_error("BuildTileJobs: output_tile must be >= step");
  }

  const int cover_w = active_rect.width;
  const int cover_h = active_rect.height;

  // Product export floor (matches Bayer/XTrans kMinProductOwned / rect axis minimum).
  const int min_product_owned = kStudentMinOwnedRectAxis;

  const auto x_spans = PlanStudentAxisSpans(
      cover_w, policy.output_tile.width, policy.step.width, policy.virtual_pad.x,
      policy.output_border.x, policy.cfa_period, policy.ragged_edge_tiles,
      kStudentRaggedMinOwnedTail, min_product_owned);
  const auto y_spans = PlanStudentAxisSpans(
      cover_h, policy.output_tile.height, policy.step.height, policy.virtual_pad.y,
      policy.output_border.y, policy.cfa_period, policy.ragged_edge_tiles,
      kStudentRaggedMinOwnedTail, min_product_owned);

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

      CudaTileJob job;
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
