//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "demosaicnet_perf_roofline.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include "decoders/processor/cuda_tile_jobs.hpp"

namespace alcedo::perf {
namespace {

constexpr std::int64_t kFloatBytes = 4;

[[nodiscard]] auto CeilDiv(const int a, const int b) -> int {
  if (b <= 0) {
    return 0;
  }
  return (a + b - 1) / b;
}

void AccumulateTotals(TopologyAccounting& acc) {
  acc.total_conv_flops    = 0;
  acc.total_all_flops     = 0;
  acc.total_bytes_read    = 0;
  acc.total_bytes_written = 0;
  for (const auto& op : acc.ops) {
    acc.total_all_flops += op.flops;
    acc.total_bytes_read += op.bytes_read;
    acc.total_bytes_written += op.bytes_written;
    if (!op.spec.is_structural) {
      acc.total_conv_flops += op.flops;
    }
  }
  acc.total_bytes_traffic = acc.total_bytes_read + acc.total_bytes_written;
  acc.arithmetic_intensity =
      acc.total_bytes_traffic > 0
          ? static_cast<double>(acc.total_conv_flops) /
                static_cast<double>(acc.total_bytes_traffic)
          : 0.0;
}

[[nodiscard]] auto MakeConvSpec(std::string name, const int n, const int cin, const int cout,
                                const int k, const int s, const int in_h, const int in_w,
                                const int groups = 1) -> ConvOpSpec {
  ConvOpSpec spec;
  spec.name   = std::move(name);
  spec.n      = n;
  spec.cin    = cin;
  spec.cout   = cout;
  spec.k_h    = k;
  spec.k_w    = k;
  spec.s_h    = s;
  spec.s_w    = s;
  spec.in_h   = in_h;
  spec.in_w   = in_w;
  spec.out_h  = Conv2dValidOut(in_h, k, s);
  spec.out_w  = Conv2dValidOut(in_w, k, s);
  spec.groups = groups;
  return spec;
}

}  // namespace

auto ToString(const RooflineTrackDecision decision) -> const char* {
  switch (decision) {
    case RooflineTrackDecision::ContinueFp32DirectKernels:
      return "continue_fp32_direct_kernels";
    case RooflineTrackDecision::EvaluateMixedPrecision:
      return "evaluate_mixed_precision";
    case RooflineTrackDecision::TopologyOrDistillRequired:
      return "topology_or_distill_required";
    case RooflineTrackDecision::InsufficientTimingData:
      return "insufficient_timing_data";
  }
  return "unknown";
}

auto Conv2dValidOut(const int in, const int k, const int s) -> int {
  if (in < k || s <= 0 || k <= 0) {
    return 0;
  }
  return (in - k) / s + 1;
}

auto Conv2dFlops(const int n, const int cin, const int cout, const int out_h, const int out_w,
                 const int k_h, const int k_w, const int groups) -> std::int64_t {
  if (n <= 0 || cin <= 0 || cout <= 0 || out_h <= 0 || out_w <= 0 || k_h <= 0 || k_w <= 0 ||
      groups <= 0) {
    return 0;
  }
  if ((cin % groups) != 0 || (cout % groups) != 0) {
    return 0;
  }
  // FLOPs = 2 * N * Cout * Hout * Wout * (Cin/groups) * kH * kW
  const std::int64_t cin_g = static_cast<std::int64_t>(cin / groups);
  return 2LL * n * cout * out_h * out_w * cin_g * k_h * k_w;
}

auto ConvTranspose2dFlops(const int n, const int cin, const int cout, const int in_h,
                          const int in_w, const int k_h, const int k_w, const int groups)
    -> std::int64_t {
  if (n <= 0 || cin <= 0 || cout <= 0 || in_h <= 0 || in_w <= 0 || k_h <= 0 || k_w <= 0 ||
      groups <= 0) {
    return 0;
  }
  if ((cin % groups) != 0 || (cout % groups) != 0) {
    return 0;
  }
  // Each input spatial site contributes (Cout/groups)*kH*kW MACs per input channel.
  const std::int64_t cout_g = static_cast<std::int64_t>(cout / groups);
  return 2LL * n * cin * in_h * in_w * cout_g * k_h * k_w;
}

auto AccountConv2dOp(const ConvOpSpec& spec) -> OpAccounting {
  OpAccounting acc;
  acc.spec = spec;
  acc.spec.is_conv_transpose = false;
  acc.spec.is_structural     = false;
  if (acc.spec.out_h <= 0 || acc.spec.out_w <= 0) {
    acc.spec.out_h = Conv2dValidOut(spec.in_h, spec.k_h, spec.s_h);
    acc.spec.out_w = Conv2dValidOut(spec.in_w, spec.k_w, spec.s_w);
  }
  acc.flops = Conv2dFlops(spec.n, spec.cin, spec.cout, acc.spec.out_h, acc.spec.out_w, spec.k_h,
                          spec.k_w, spec.groups);
  const std::int64_t act_in =
      static_cast<std::int64_t>(spec.n) * spec.cin * spec.in_h * spec.in_w * kFloatBytes;
  const std::int64_t weights = static_cast<std::int64_t>(spec.cout) * (spec.cin / spec.groups) *
                               spec.k_h * spec.k_w * kFloatBytes;
  const std::int64_t bias = static_cast<std::int64_t>(spec.cout) * kFloatBytes;
  const std::int64_t act_out =
      static_cast<std::int64_t>(spec.n) * spec.cout * acc.spec.out_h * acc.spec.out_w * kFloatBytes;
  acc.bytes_read    = act_in + weights + bias;
  acc.bytes_written = act_out;
  const auto traffic = acc.bytes_read + acc.bytes_written;
  acc.arithmetic_intensity =
      traffic > 0 ? static_cast<double>(acc.flops) / static_cast<double>(traffic) : 0.0;
  return acc;
}

auto AccountConvTransposeOp(const ConvOpSpec& spec) -> OpAccounting {
  OpAccounting acc;
  acc.spec = spec;
  acc.spec.is_conv_transpose = true;
  acc.spec.is_structural     = false;
  // unpack_mosaick: k=s=2, pad=0 → Hout = Hin * 2
  if (acc.spec.out_h <= 0 || acc.spec.out_w <= 0) {
    acc.spec.out_h = (spec.in_h - 1) * spec.s_h + spec.k_h;
    acc.spec.out_w = (spec.in_w - 1) * spec.s_w + spec.k_w;
  }
  acc.flops = ConvTranspose2dFlops(spec.n, spec.cin, spec.cout, spec.in_h, spec.in_w, spec.k_h,
                                   spec.k_w, spec.groups);
  const std::int64_t act_in =
      static_cast<std::int64_t>(spec.n) * spec.cin * spec.in_h * spec.in_w * kFloatBytes;
  const std::int64_t weights = static_cast<std::int64_t>(spec.cin) * (spec.cout / spec.groups) *
                               spec.k_h * spec.k_w * kFloatBytes;
  const std::int64_t bias = static_cast<std::int64_t>(spec.cout) * kFloatBytes;
  const std::int64_t act_out =
      static_cast<std::int64_t>(spec.n) * spec.cout * acc.spec.out_h * acc.spec.out_w * kFloatBytes;
  acc.bytes_read    = act_in + weights + bias;
  acc.bytes_written = act_out;
  const auto traffic = acc.bytes_read + acc.bytes_written;
  acc.arithmetic_intensity =
      traffic > 0 ? static_cast<double>(acc.flops) / static_cast<double>(traffic) : 0.0;
  return acc;
}

auto AccountElemwiseMul(const std::string_view name, const int n, const int c, const int h,
                        const int w) -> OpAccounting {
  OpAccounting acc;
  acc.spec.name           = std::string(name);
  acc.spec.n              = n;
  acc.spec.cin            = c;
  acc.spec.cout           = c;
  acc.spec.in_h           = h;
  acc.spec.in_w           = w;
  acc.spec.out_h          = h;
  acc.spec.out_w          = w;
  acc.spec.is_structural  = true;
  const std::int64_t elems =
      static_cast<std::int64_t>(n) * c * h * w;
  acc.flops         = elems;  // one mul per element
  acc.bytes_read    = 2 * elems * kFloatBytes;
  acc.bytes_written = elems * kFloatBytes;
  const auto traffic = acc.bytes_read + acc.bytes_written;
  acc.arithmetic_intensity =
      traffic > 0 ? static_cast<double>(acc.flops) / static_cast<double>(traffic) : 0.0;
  return acc;
}

auto BuildBayerTileAccounting(const int owned_w, const int owned_h, const int batch)
    -> TopologyAccounting {
  if (owned_w < 2 || owned_h < 2 || batch < 1) {
    throw std::runtime_error("BuildBayerTileAccounting: invalid owned size/batch");
  }
  // Student bayer_s24_d8 product tile: input = owned + 2*border31 (1086 for 1024).
  constexpr int kDepth      = 8;
  constexpr int kWidth      = 24;
  constexpr int kPackFactor = 2;
  constexpr int kTileBorder = 31;
  const int     hin         = owned_h + 2 * kTileBorder;
  const int     win         = owned_w + 2 * kTileBorder;
  const int n   = batch;

  TopologyAccounting acc;
  acc.kind           = DemosaicNetTopologyKind::Bayer;
  acc.tile_input_h   = hin;
  acc.tile_input_w   = win;
  acc.tile_output_h  = owned_h;
  acc.tile_output_w  = owned_w;
  acc.batch          = batch;

  {
    auto spec = MakeConvSpec("pack", n, 3, 4, 2, 2, hin, win);
    acc.ops.push_back(AccountConv2dOp(spec));
  }
  int h = hin / kPackFactor;
  int w = win / kPackFactor;

  {
    auto spec = MakeConvSpec("trunk_1", n, 4, kWidth, 3, 1, h, w);
    acc.ops.push_back(AccountConv2dOp(spec));
    h = spec.out_h;
    w = spec.out_w;
  }
  for (int i = 2; i <= kDepth; ++i) {
    auto spec = MakeConvSpec("trunk_" + std::to_string(i), n, kWidth, kWidth, 3, 1, h, w);
    acc.ops.push_back(AccountConv2dOp(spec));
    h = spec.out_h;
    w = spec.out_w;
  }
  {
    auto spec = MakeConvSpec("residual", n, kWidth, 12, 1, 1, h, w);
    acc.ops.push_back(AccountConv2dOp(spec));
  }
  {
    ConvOpSpec spec;
    spec.name           = "unpack";
    spec.n              = n;
    spec.cin            = 12;
    spec.cout           = 3;
    spec.k_h = spec.k_w = 2;
    spec.s_h = spec.s_w = 2;
    spec.in_h           = h;
    spec.in_w           = w;
    spec.out_h          = h * 2;
    spec.out_w          = w * 2;
    spec.groups         = 3;
    acc.ops.push_back(AccountConvTransposeOp(spec));
  }
  const int uh = h * 2;
  const int uw = w * 2;
  {
    auto spec = MakeConvSpec("post_conv", n, 6, kWidth, 3, 1, uh, uw);
    acc.ops.push_back(AccountConv2dOp(spec));
    h = spec.out_h;
    w = spec.out_w;
  }
  {
    auto spec = MakeConvSpec("output", n, kWidth, 3, 1, 1, h, w);
    acc.ops.push_back(AccountConv2dOp(spec));
    h = spec.out_h;
    w = spec.out_w;
  }

  const int natural_h = hin - (2 * kPackFactor * kDepth + 2);
  const int natural_w = win - (2 * kPackFactor * kDepth + 2);
  if (h != natural_h || w != natural_w) {
    throw std::runtime_error("BuildBayerTileAccounting: natural spatial mismatch");
  }
  // Export center-crop is structural (no MACs).
  if (owned_h > natural_h || owned_w > natural_w) {
    throw std::runtime_error("BuildBayerTileAccounting: owned size exceeds natural size");
  }

  AccumulateTotals(acc);
  return acc;
}

auto BuildBayerTileAccounting(const int tile_output, const int batch) -> TopologyAccounting {
  return BuildBayerTileAccounting(tile_output, tile_output, batch);
}

auto BuildXTransTileAccounting(const int owned_w, const int owned_h, const int batch)
    -> TopologyAccounting {
  if (owned_w < 2 || owned_h < 2 || batch < 1) {
    throw std::runtime_error("BuildXTransTileAccounting: invalid owned size/batch");
  }
  // Student xtrans_p2_s32_d4 product tile: input = owned + 2*border12 (1048 for 1024).
  constexpr int kDepth      = 4;
  constexpr int kWidth      = 32;
  constexpr int kPackFactor = 2;
  constexpr int kTileBorder = 12;
  const int     hin         = owned_h + 2 * kTileBorder;
  const int     win         = owned_w + 2 * kTileBorder;
  const int n   = batch;

  TopologyAccounting acc;
  acc.kind          = DemosaicNetTopologyKind::XTrans;
  acc.tile_input_h  = hin;
  acc.tile_input_w  = win;
  acc.tile_output_h = owned_h;
  acc.tile_output_w = owned_w;
  acc.batch         = batch;

  {
    auto spec = MakeConvSpec("pack", n, 3, 12, 2, 2, hin, win);
    acc.ops.push_back(AccountConv2dOp(spec));
  }
  int h = hin / kPackFactor;
  int w = win / kPackFactor;
  {
    auto spec = MakeConvSpec("trunk_1", n, 12, kWidth, 3, 1, h, w);
    acc.ops.push_back(AccountConv2dOp(spec));
    h = spec.out_h;
    w = spec.out_w;
  }
  for (int i = 2; i <= kDepth; ++i) {
    auto spec = MakeConvSpec("trunk_" + std::to_string(i), n, kWidth, kWidth, 3, 1, h, w);
    acc.ops.push_back(AccountConv2dOp(spec));
    h = spec.out_h;
    w = spec.out_w;
  }
  {
    auto spec = MakeConvSpec("residual", n, kWidth, 12, 1, 1, h, w);
    acc.ops.push_back(AccountConv2dOp(spec));
  }
  {
    ConvOpSpec spec;
    spec.name           = "unpack";
    spec.n              = n;
    spec.cin            = 12;
    spec.cout           = 3;
    spec.k_h = spec.k_w = 2;
    spec.s_h = spec.s_w = 2;
    spec.in_h           = h;
    spec.in_w           = w;
    spec.out_h          = h * 2;
    spec.out_w          = w * 2;
    spec.groups         = 3;
    acc.ops.push_back(AccountConvTransposeOp(spec));
  }
  const int uh = h * 2;
  const int uw = w * 2;
  {
    auto spec = MakeConvSpec("post_conv", n, 6, kWidth, 3, 1, uh, uw);
    acc.ops.push_back(AccountConv2dOp(spec));
    h = spec.out_h;
    w = spec.out_w;
  }
  {
    auto spec = MakeConvSpec("output", n, kWidth, 3, 1, 1, h, w);
    acc.ops.push_back(AccountConv2dOp(spec));
    h = spec.out_h;
    w = spec.out_w;
  }

  const int natural_h = hin - (2 * kPackFactor * kDepth + 2);
  const int natural_w = win - (2 * kPackFactor * kDepth + 2);
  if (h != natural_h || w != natural_w) {
    throw std::runtime_error("BuildXTransTileAccounting: natural spatial mismatch");
  }
  if (owned_h > natural_h || owned_w > natural_w) {
    throw std::runtime_error("BuildXTransTileAccounting: owned size exceeds natural size");
  }

  AccumulateTotals(acc);
  return acc;
}

auto BuildXTransTileAccounting(const int tile_output, const int batch) -> TopologyAccounting {
  return BuildXTransTileAccounting(tile_output, tile_output, batch);
}

auto BuildTileAccounting(const DemosaicNetTopologyKind kind, const int tile_output,
                         const int batch) -> TopologyAccounting {
  return kind == DemosaicNetTopologyKind::Bayer ? BuildBayerTileAccounting(tile_output, batch)
                                                : BuildXTransTileAccounting(tile_output, batch);
}

auto BuildTileAccounting(const DemosaicNetTopologyKind kind, const int owned_w, const int owned_h,
                         const int batch) -> TopologyAccounting {
  return kind == DemosaicNetTopologyKind::Bayer ? BuildBayerTileAccounting(owned_w, owned_h, batch)
                                                : BuildXTransTileAccounting(owned_w, owned_h, batch);
}

// Match product BuildTileJobs student grid: count gx,gy while
// model_out = g*step - pad + border is still inside the cover.
[[nodiscard]] auto CountStudentProductTiles(const int cover_w, const int cover_h, const int step_x,
                                            const int step_y, const int pad_x, const int pad_y,
                                            const int border_x, const int border_y)
    -> std::tuple<int, int, int> {
  int tiles_x = 0;
  int tiles_y = 0;
  for (int gy = 0;; ++gy) {
    const int model_out_y = gy * step_y - pad_y + border_y;
    if (model_out_y >= cover_h) {
      break;
    }
    tiles_y = gy + 1;
    int row_tiles = 0;
    for (int gx = 0;; ++gx) {
      const int model_out_x = gx * step_x - pad_x + border_x;
      if (model_out_x >= cover_w) {
        break;
      }
      row_tiles = gx + 1;
    }
    tiles_x = std::max(tiles_x, row_tiles);
  }
  return {tiles_x, tiles_y, tiles_x * tiles_y};
}

auto EstimateFullFrameWork(const DemosaicNetTopologyKind kind, const int cover_w,
                           const int cover_h, const int owned_w, const int owned_h,
                           const int tile_count_override) -> FullFrameWorkEstimate {
  if (cover_w <= 0 || cover_h <= 0 || owned_w <= 0 || owned_h <= 0) {
    throw std::runtime_error("EstimateFullFrameWork: invalid dimensions");
  }

  FullFrameWorkEstimate est;
  est.tile_inner   = owned_w;  // historical field: report width; see paid pixels for area
  est.tile_owned_w = owned_w;
  est.tile_owned_h = owned_h;
  // Product student policies are fixed 1024 owned (zero-arg makers only).
  const detail::CudaTilePolicy policy = kind == DemosaicNetTopologyKind::Bayer
                                            ? detail::MakeBayerStudentTilePolicy()
                                            : detail::MakeXTransStudentTilePolicy();
  est.tile_step_x     = policy.step.width;
  est.tile_step_y     = policy.step.height;
  est.tile_step       = policy.step.width;  // historical alias (X)
  est.virtual_pad     = policy.virtual_pad.x;
  est.source_border   = policy.output_border.x;
  est.overlap_x       = std::max(0, owned_w - est.tile_step_x);
  est.overlap_y       = std::max(0, owned_h - est.tile_step_y);
  est.first_model_out_x = -est.virtual_pad + est.source_border;  // Bayer -1, X-Trans 0
  est.first_model_out_y = est.first_model_out_x;
  est.cover_width   = cover_w;
  est.cover_height  = cover_h;
  est.active_width  = cover_w;
  est.active_height = cover_h;

  const auto [tiles_x, tiles_y, planner_count] = CountStudentProductTiles(
      cover_w, cover_h, est.tile_step_x, est.tile_step_y, est.virtual_pad, est.virtual_pad,
      est.source_border, est.source_border);
  est.tiles_x    = tiles_x;
  est.tiles_y    = tiles_y;
  est.tile_count = tile_count_override > 0 ? tile_count_override : planner_count;
  if (est.tile_count <= 0) {
    // Degenerate cover smaller than first model origin reach — fall back to 1×1 ceil.
    est.tiles_x    = std::max(1, CeilDiv(cover_w, est.tile_step_x));
    est.tiles_y    = std::max(1, CeilDiv(cover_h, est.tile_step_y));
    est.tile_count = est.tiles_x * est.tiles_y;
  }

  est.per_tile = BuildTileAccounting(kind, owned_w, owned_h, 1);

  const auto scale = static_cast<std::int64_t>(est.tile_count);
  est.full_conv_flops    = est.per_tile.total_conv_flops * scale;
  est.full_all_flops     = est.per_tile.total_all_flops * scale;
  est.full_bytes_read    = est.per_tile.total_bytes_read * scale;
  est.full_bytes_written = est.per_tile.total_bytes_written * scale;
  est.full_bytes_traffic = est.per_tile.total_bytes_traffic * scale;

  est.active_output_megapixels =
      (static_cast<double>(cover_w) * static_cast<double>(cover_h)) / 1.0e6;
  est.paid_tile_output_megapixels =
      (static_cast<double>(est.tile_count) * static_cast<double>(owned_w) *
       static_cast<double>(owned_h)) /
      1.0e6;
  est.halo_work_factor =
      est.active_output_megapixels > 0.0
          ? est.paid_tile_output_megapixels / est.active_output_megapixels
          : 1.0;
  return est;
}

auto EstimateFullFrameWork(const DemosaicNetTopologyKind kind, const int cover_w,
                           const int cover_h, const int tile_inner) -> FullFrameWorkEstimate {
  return EstimateFullFrameWork(kind, cover_w, cover_h, tile_inner, tile_inner,
                               /*tile_count_override=*/-1);
}

auto EstimateDeviceComputeEnvelope(const DeviceInfo& info) -> DeviceComputeEnvelope {
  DeviceComputeEnvelope env;
  env.name                   = info.name;
  env.compute_major          = info.compute_major;
  env.compute_minor          = info.compute_minor;
  env.multi_processor_count  = info.multi_processor_count;
  env.clock_rate_khz         = info.clock_rate_khz;

  // FP32 CUDA cores / SM (architecture tables; consumer-leaning defaults).
  const int major = info.compute_major;
  const int minor = info.compute_minor;
  int cores = 64;
  if (major == 6) {
    cores = 128;  // Pascal
  } else if (major == 7 && minor == 0) {
    cores = 64;  // Volta
  } else if (major == 7 && minor >= 5) {
    cores = 64;  // Turing
  } else if (major == 8 && minor == 0) {
    cores = 64;  // A100
  } else if (major == 8) {
    cores = 128;  // GA10x / Ada
  } else if (major >= 9) {
    cores = 128;  // Hopper / Blackwell-class default
  }
  env.fp32_cores_per_sm = cores;

  const double clock_ghz =
      info.clock_rate_khz > 0 ? static_cast<double>(info.clock_rate_khz) / 1.0e6 : 0.0;
  // peak FP32 TFLOP/s = SM * cores/SM * 2 (FMA) * GHz
  env.peak_fp32_tflops = static_cast<double>(info.multi_processor_count) *
                         static_cast<double>(cores) * 2.0 * clock_ghz / 1.0e3;

  // Tensor Core peaks relative to dense FP32 (order-of-magnitude architectural ratios).
  // These are *not* guaranteed product numbers; used only for the feasibility gate.
  if (major >= 8) {
    // Ampere+: TF32 tensor ≈ 2× FP32 peak on many GA10x parts; Ada often closer to 1×
    // for dense TF32 vs FP32 marketing — use 2× as optimistic TC upper bound for gate.
    const double tf32_mult = (major == 8 && minor >= 9) ? 1.0 : 2.0;
    env.peak_tf32_tflops    = env.peak_fp32_tflops * tf32_mult;
    env.peak_fp16_tc_tflops = env.peak_fp32_tflops * 4.0;  // dense FP16 TC rough ratio
  } else if (major == 7) {
    env.peak_tf32_tflops    = 0.0;
    env.peak_fp16_tc_tflops = env.peak_fp32_tflops * 4.0;
  }

  // Custom-kernel sustained fractions (well below cuBLAS/cuDNN peaks).
  constexpr double kSustainedFp32Frac = 0.55;
  constexpr double kSustainedTcFrac   = 0.45;
  env.sustained_fp32_tflops     = env.peak_fp32_tflops * kSustainedFp32Frac;
  env.sustained_tf32_tflops     = env.peak_tf32_tflops * kSustainedTcFrac;
  env.sustained_fp16_tc_tflops  = env.peak_fp16_tc_tflops * kSustainedTcFrac;

  std::ostringstream notes;
  notes << "peak_fp32=SM*cores*2*GHz; sustained_fp32=" << kSustainedFp32Frac
        << "*peak; sustained_tc=" << kSustainedTcFrac
        << "*peak_tc. TC ratios are architectural estimates for the gate, not measured.";
  env.notes = notes.str();
  return env;
}

auto BuildRooflineReport(const FullFrameWorkEstimate& work, const DeviceComputeEnvelope& device,
                         const double neural_median_ms, const double legacy_median_ms,
                         const double stretch_target_ms) -> RooflineReport {
  RooflineReport r;
  r.work               = work;
  r.device             = device;
  r.neural_median_ms   = neural_median_ms;
  r.legacy_median_ms   = legacy_median_ms;
  r.stretch_target_ms  = stretch_target_ms;

  const double flops = static_cast<double>(work.full_conv_flops);
  const double bytes = static_cast<double>(work.full_bytes_traffic);

  auto tflops_for_ms = [&](const double ms) -> double {
    if (ms <= 0.0 || flops <= 0.0) {
      return 0.0;
    }
    return (flops / (ms * 1.0e-3)) / 1.0e12;
  };

  r.effective_tflops            = tflops_for_ms(neural_median_ms);
  r.required_tflops_for_legacy  = tflops_for_ms(legacy_median_ms);
  r.required_tflops_for_stretch = tflops_for_ms(stretch_target_ms);

  auto ms_at_tflops = [&](const double tflops) -> double {
    if (tflops <= 0.0 || flops <= 0.0) {
      return 0.0;
    }
    return (flops / (tflops * 1.0e12)) * 1.0e3;
  };
  r.best_case_fp32_ms    = ms_at_tflops(device.sustained_fp32_tflops);
  r.best_case_tf32_ms    = ms_at_tflops(device.sustained_tf32_tflops);
  r.best_case_fp16_tc_ms = ms_at_tflops(device.sustained_fp16_tc_tflops);

  if (neural_median_ms > 0.0 && bytes > 0.0) {
    r.effective_bandwidth_gbs = (bytes / (neural_median_ms * 1.0e-3)) / 1.0e9;
  }
  if (device.peak_fp32_tflops > 0.0 && r.effective_tflops > 0.0) {
    r.util_vs_peak_fp32 = r.effective_tflops / device.peak_fp32_tflops;
  }
  if (device.sustained_fp32_tflops > 0.0 && r.effective_tflops > 0.0) {
    r.util_vs_sustained_fp32 = r.effective_tflops / device.sustained_fp32_tflops;
  }
  r.interim_fp32_kernel_headroom =
      r.effective_tflops > 0.0 && device.sustained_fp32_tflops > 0.0 &&
      r.effective_tflops < device.sustained_fp32_tflops * 0.70;

  // Decision gate (Phase 8.2):
  // 1. Prefer required-for-Legacy when Legacy timing is present; else stretch.
  // 2. FP32 track if required rate is within sustained FP32 (with small slack).
  // 3. Else mixed precision if within sustained TF32/FP16 TC.
  // 4. Else topology/distill.
  // Tile concurrency alone is never a feasibility argument (total work fixed).
  const double required =
      r.required_tflops_for_legacy > 0.0 ? r.required_tflops_for_legacy
                                         : r.required_tflops_for_stretch;
  if (required <= 0.0 || flops <= 0.0) {
    r.decision = RooflineTrackDecision::InsufficientTimingData;
    r.decision_rationale =
        "Missing timing and/or zero FLOP budget; analytical work totals only.";
    return r;
  }

  constexpr double kFp32Slack = 1.15;  // allow ~15% optimism vs sustained custom FP32
  const double fp32_budget    = device.sustained_fp32_tflops * kFp32Slack;
  const double tc_budget =
      std::max(device.sustained_tf32_tflops, device.sustained_fp16_tc_tflops);

  std::ostringstream rationale;
  rationale << std::fixed << std::setprecision(3);
  rationale << "full_conv_FLOPs=" << flops << " (" << (flops / 1.0e12) << " TFLOP); "
            << "required=" << required << " TFLOP/s "
            << (r.required_tflops_for_legacy > 0.0 ? "for Legacy parity" : "for 100ms stretch")
            << "; sustained_fp32=" << device.sustained_fp32_tflops
            << " TFLOP/s; sustained_tc≈" << tc_budget << " TFLOP/s"
            << "; best_case_fp32_ms=" << r.best_case_fp32_ms
            << "; best_case_fp16_tc_ms=" << r.best_case_fp16_tc_ms
            << "; halo_work_factor=" << work.halo_work_factor
            << "; tile_count=" << work.tile_count << ".";

  if (required <= fp32_budget) {
    r.decision = RooflineTrackDecision::ContinueFp32DirectKernels;
    rationale << " Decision: continue FP32 direct-kernel track — required rate is within "
                 "sustained FP32 envelope (slack "
              << kFp32Slack << ").";
  } else if (tc_budget > 0.0 && required <= tc_budget * 1.10) {
    r.decision = RooflineTrackDecision::EvaluateMixedPrecision;
    rationale << " Decision: evaluate FP16/BF16/TF32 Tensor Core with FP32 accumulate — "
                 "FP32 sustained cannot cover required rate; TC envelope can.";
  } else {
    r.decision = RooflineTrackDecision::TopologyOrDistillRequired;
    rationale << " Decision: topology reduction / distillation required for Legacy-parity "
                 "and/or the 100ms stretch — even optimistic TC sustained cannot cover the "
                 "required rate for this fixed FLOP budget. Tile concurrency does not "
                 "reduce total work on one GPU.";
    if (tc_budget > 0.0 && r.best_case_fp16_tc_ms > 0.0 &&
        (legacy_median_ms <= 0.0 || r.best_case_fp16_tc_ms < legacy_median_ms * 3.0)) {
      rationale << " Mixed precision remains a useful partial lever (best_case_fp16_tc_ms≈"
                << r.best_case_fp16_tc_ms << ") but is not alone enough for parity.";
    }
  }
  if (r.interim_fp32_kernel_headroom) {
    rationale << " Interim: measured effective TFLOP/s is below 70% of sustained FP32, so "
                 "kernel/async/graph work still closes part of the gap before precision/"
                 "topology changes.";
  }
  r.decision_rationale = rationale.str();
  return r;
}

void AppendJsonRooflineReport(std::string& out, const std::string_view key,
                              const RooflineReport& report, const bool trailing_comma) {
  out += "\"";
  out += key;
  out += "\":{";

  AppendJsonKeyString(out, "topology",
                      report.work.per_tile.kind == DemosaicNetTopologyKind::Bayer ? "bayer"
                                                                                  : "xtrans");
  AppendJsonKeyInt(out, "tile_inner", report.work.tile_inner);
  AppendJsonKeyInt(out, "tile_step", report.work.tile_step);
  AppendJsonKeyInt(out, "virtual_pad", report.work.virtual_pad);
  AppendJsonKeyInt(out, "source_border", report.work.source_border);
  AppendJsonKeyInt(out, "overlap_x", report.work.overlap_x);
  AppendJsonKeyInt(out, "overlap_y", report.work.overlap_y);
  AppendJsonKeyInt(out, "first_model_out_x", report.work.first_model_out_x);
  AppendJsonKeyInt(out, "first_model_out_y", report.work.first_model_out_y);
  AppendJsonKeyInt(out, "tile_input_h", report.work.per_tile.tile_input_h);
  AppendJsonKeyInt(out, "tile_input_w", report.work.per_tile.tile_input_w);
  AppendJsonKeyInt(out, "tile_count", report.work.tile_count);
  AppendJsonKeyInt(out, "tiles_x", report.work.tiles_x);
  AppendJsonKeyInt(out, "tiles_y", report.work.tiles_y);
  AppendJsonKeyInt(out, "cover_width", report.work.cover_width);
  AppendJsonKeyInt(out, "cover_height", report.work.cover_height);
  AppendJsonKeyInt(out, "active_width", report.work.active_width);
  AppendJsonKeyInt(out, "active_height", report.work.active_height);
  AppendJsonKeyNumber(out, "halo_work_factor", report.work.halo_work_factor);
  AppendJsonKeyNumber(out, "active_output_megapixels", report.work.active_output_megapixels);
  AppendJsonKeyNumber(out, "paid_tile_output_megapixels",
                      report.work.paid_tile_output_megapixels);

  AppendJsonKeyInt(out, "per_tile_conv_flops", report.work.per_tile.total_conv_flops);
  AppendJsonKeyInt(out, "per_tile_all_flops", report.work.per_tile.total_all_flops);
  AppendJsonKeyInt(out, "per_tile_bytes_read", report.work.per_tile.total_bytes_read);
  AppendJsonKeyInt(out, "per_tile_bytes_written", report.work.per_tile.total_bytes_written);
  AppendJsonKeyNumber(out, "per_tile_arithmetic_intensity",
                      report.work.per_tile.arithmetic_intensity);

  AppendJsonKeyInt(out, "full_conv_flops", report.work.full_conv_flops);
  AppendJsonKeyInt(out, "full_all_flops", report.work.full_all_flops);
  AppendJsonKeyInt(out, "full_bytes_read", report.work.full_bytes_read);
  AppendJsonKeyInt(out, "full_bytes_written", report.work.full_bytes_written);
  AppendJsonKeyInt(out, "full_bytes_traffic", report.work.full_bytes_traffic);
  AppendJsonKeyNumber(out, "full_conv_tflop",
                      static_cast<double>(report.work.full_conv_flops) / 1.0e12);

  out += "\"layers\":[";
  for (std::size_t i = 0; i < report.work.per_tile.ops.size(); ++i) {
    const auto& op = report.work.per_tile.ops[i];
    out += "{";
    AppendJsonKeyString(out, "name", op.spec.name);
    AppendJsonKeyInt(out, "cin", op.spec.cin);
    AppendJsonKeyInt(out, "cout", op.spec.cout);
    AppendJsonKeyInt(out, "k_h", op.spec.k_h);
    AppendJsonKeyInt(out, "k_w", op.spec.k_w);
    AppendJsonKeyInt(out, "s_h", op.spec.s_h);
    AppendJsonKeyInt(out, "s_w", op.spec.s_w);
    AppendJsonKeyInt(out, "in_h", op.spec.in_h);
    AppendJsonKeyInt(out, "in_w", op.spec.in_w);
    AppendJsonKeyInt(out, "out_h", op.spec.out_h);
    AppendJsonKeyInt(out, "out_w", op.spec.out_w);
    AppendJsonKeyInt(out, "groups", op.spec.groups);
    AppendJsonKeyBool(out, "is_conv_transpose", op.spec.is_conv_transpose);
    AppendJsonKeyBool(out, "is_structural", op.spec.is_structural);
    AppendJsonKeyInt(out, "flops", op.flops);
    AppendJsonKeyInt(out, "bytes_read", op.bytes_read);
    AppendJsonKeyInt(out, "bytes_written", op.bytes_written);
    AppendJsonKeyNumber(out, "arithmetic_intensity", op.arithmetic_intensity, false);
    out += (i + 1 < report.work.per_tile.ops.size()) ? "}," : "}";
  }
  out += "],";

  out += "\"device_envelope\":{";
  AppendJsonKeyString(out, "name", report.device.name);
  AppendJsonKeyString(out, "compute_capability",
                      std::to_string(report.device.compute_major) + "." +
                          std::to_string(report.device.compute_minor));
  AppendJsonKeyInt(out, "sm_count", report.device.multi_processor_count);
  AppendJsonKeyInt(out, "fp32_cores_per_sm", report.device.fp32_cores_per_sm);
  AppendJsonKeyInt(out, "clock_rate_khz", report.device.clock_rate_khz);
  AppendJsonKeyNumber(out, "peak_fp32_tflops", report.device.peak_fp32_tflops);
  AppendJsonKeyNumber(out, "peak_tf32_tflops", report.device.peak_tf32_tflops);
  AppendJsonKeyNumber(out, "peak_fp16_tc_tflops", report.device.peak_fp16_tc_tflops);
  AppendJsonKeyNumber(out, "sustained_fp32_tflops", report.device.sustained_fp32_tflops);
  AppendJsonKeyNumber(out, "sustained_tf32_tflops", report.device.sustained_tf32_tflops);
  AppendJsonKeyNumber(out, "sustained_fp16_tc_tflops", report.device.sustained_fp16_tc_tflops);
  AppendJsonKeyString(out, "notes", report.device.notes, false);
  out += "},";

  AppendJsonKeyNumber(out, "neural_median_ms", report.neural_median_ms);
  AppendJsonKeyNumber(out, "legacy_median_ms", report.legacy_median_ms);
  AppendJsonKeyNumber(out, "stretch_target_ms", report.stretch_target_ms);
  AppendJsonKeyNumber(out, "effective_tflops", report.effective_tflops);
  AppendJsonKeyNumber(out, "effective_bandwidth_gbs", report.effective_bandwidth_gbs);
  AppendJsonKeyNumber(out, "required_tflops_for_legacy", report.required_tflops_for_legacy);
  AppendJsonKeyNumber(out, "required_tflops_for_stretch", report.required_tflops_for_stretch);
  AppendJsonKeyNumber(out, "best_case_fp32_ms", report.best_case_fp32_ms);
  AppendJsonKeyNumber(out, "best_case_tf32_ms", report.best_case_tf32_ms);
  AppendJsonKeyNumber(out, "best_case_fp16_tc_ms", report.best_case_fp16_tc_ms);
  AppendJsonKeyNumber(out, "util_vs_peak_fp32", report.util_vs_peak_fp32);
  AppendJsonKeyNumber(out, "util_vs_sustained_fp32", report.util_vs_sustained_fp32);
  AppendJsonKeyBool(out, "interim_fp32_kernel_headroom", report.interim_fp32_kernel_headroom);
  AppendJsonKeyString(out, "decision", ToString(report.decision));
  AppendJsonKeyString(out, "decision_rationale", report.decision_rationale, false);

  out += trailing_comma ? "}," : "}";
}

void PrintRooflineReport(const RooflineReport& report) {
  const auto& w = report.work;
  const auto& d = report.device;
  std::cout << std::fixed << std::setprecision(3);
  std::cout << "\n=== Roofline / feasibility (Phase 8F student product) ===\n";
  std::cout << "  topology="
            << (w.per_tile.kind == DemosaicNetTopologyKind::Bayer ? "bayer_s24_d8"
                                                                  : "xtrans_p2_s32_d4")
            << "  tile_in=" << w.per_tile.tile_input_h << "x" << w.per_tile.tile_input_w
            << "  tile_out=" << w.tile_inner << "x" << w.tile_inner
            << "  step=" << w.tile_step << "  pad=" << w.virtual_pad
            << "  border=" << w.source_border << "  overlap=" << w.overlap_x << "\n";
  std::cout << "  cover=" << w.cover_width << "x" << w.cover_height
            << "  first_model_out=(" << w.first_model_out_x << "," << w.first_model_out_y << ")"
            << "  tiles=" << w.tiles_x << "x" << w.tiles_y << " (" << w.tile_count << ")"
            << "  halo_work_factor=" << w.halo_work_factor << "\n";
  std::cout << "  per_tile conv FLOPs=" << w.per_tile.total_conv_flops
            << "  traffic_bytes=" << w.per_tile.total_bytes_traffic
            << "  AI=" << w.per_tile.arithmetic_intensity << " FLOP/B\n";
  std::cout << "  full-frame conv FLOPs=" << w.full_conv_flops << " ("
            << (static_cast<double>(w.full_conv_flops) / 1.0e12) << " TFLOP)"
            << "  traffic=" << (static_cast<double>(w.full_bytes_traffic) / 1.0e9) << " GB\n";

  std::cout << "  device " << d.name << " CC" << d.compute_major << "." << d.compute_minor
            << "  peak_fp32=" << d.peak_fp32_tflops << "  sustained_fp32=" << d.sustained_fp32_tflops
            << "  peak_tf32=" << d.peak_tf32_tflops
            << "  sustained_tf32=" << d.sustained_tf32_tflops << " TFLOP/s\n";

  if (report.neural_median_ms > 0.0) {
    std::cout << "  neural_median=" << report.neural_median_ms
              << " ms  effective=" << report.effective_tflops << " TFLOP/s  "
              << report.effective_bandwidth_gbs << " GB/s"
              << "  util_peak_fp32=" << (report.util_vs_peak_fp32 * 100.0) << "%"
              << "  util_sustained_fp32=" << (report.util_vs_sustained_fp32 * 100.0) << "%\n";
  }
  if (report.legacy_median_ms > 0.0) {
    std::cout << "  legacy_median=" << report.legacy_median_ms
              << " ms  required_for_legacy=" << report.required_tflops_for_legacy << " TFLOP/s\n";
  }
  std::cout << "  required_for_" << report.stretch_target_ms
            << "ms_stretch=" << report.required_tflops_for_stretch << " TFLOP/s\n";
  std::cout << "  best_case_ms fp32=" << report.best_case_fp32_ms
            << "  tf32=" << report.best_case_tf32_ms
            << "  fp16_tc=" << report.best_case_fp16_tc_ms
            << "  interim_fp32_headroom="
            << (report.interim_fp32_kernel_headroom ? "yes" : "no") << "\n";

  std::cout << "  decision=" << ToString(report.decision) << "\n";
  std::cout << "  rationale: " << report.decision_rationale << "\n";

  // Compact top layers by FLOP share (per tile).
  if (!w.per_tile.ops.empty() && w.per_tile.total_conv_flops > 0) {
    std::vector<const OpAccounting*> ranked;
    ranked.reserve(w.per_tile.ops.size());
    for (const auto& op : w.per_tile.ops) {
      if (!op.spec.is_structural) {
        ranked.push_back(&op);
      }
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const OpAccounting* a, const OpAccounting* b) { return a->flops > b->flops; });
    std::cout << "  top layers (per-tile FLOP share):\n";
    const int show = std::min<int>(6, static_cast<int>(ranked.size()));
    for (int i = 0; i < show; ++i) {
      const auto* op = ranked[static_cast<std::size_t>(i)];
      const double share =
          100.0 * static_cast<double>(op->flops) /
          static_cast<double>(w.per_tile.total_conv_flops);
      std::cout << "    " << std::setw(14) << std::left << op->spec.name << std::right
                << "  " << op->spec.cin << "->" << op->spec.cout << " "
                << op->spec.in_h << "x" << op->spec.in_w << "->" << op->spec.out_h << "x"
                << op->spec.out_w << "  " << std::setprecision(1) << share << "%  AI="
                << std::setprecision(2) << op->arithmetic_intensity << "\n";
    }
  }
}

}  // namespace alcedo::perf
