//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "demosaicnet_perf_metrics.hpp"

namespace alcedo::perf {

// Phase 8.2: exact FLOP / traffic accounting for the hard-coded DemosaicNet
// topologies and a full-frame roofline decision gate. Not a runtime graph
// assembler — layer lists mirror BayerDemosaicNet / XTransDemosaicNet Forward.

enum class DemosaicNetTopologyKind { Bayer, XTrans };

enum class RooflineTrackDecision {
  ContinueFp32DirectKernels,   // sustained FP32 can close Legacy gap in principle
  EvaluateMixedPrecision,      // FP32 peak insufficient; TC / FP16 / TF32 plausible
  TopologyOrDistillRequired,   // even TC peak cannot reach Legacy / 100 ms
  InsufficientTimingData,      // analytical totals only (no measured interval)
};

[[nodiscard]] auto ToString(RooflineTrackDecision decision) -> const char*;

struct ConvOpSpec {
  std::string name;
  int         n    = 1;
  int         cin  = 0;
  int         cout = 0;
  int         k_h  = 1;
  int         k_w  = 1;
  int         s_h  = 1;
  int         s_w  = 1;
  int         in_h = 0;
  int         in_w = 0;
  int         out_h = 0;
  int         out_w = 0;
  int         groups = 1;
  bool        is_conv_transpose = false;
  bool        is_structural     = false;  // Mul / crop / concat — traffic only or elemwise
};

struct OpAccounting {
  ConvOpSpec    spec;
  std::int64_t  flops          = 0;
  std::int64_t  bytes_read     = 0;  // activations + weights + bias (FP32 model)
  std::int64_t  bytes_written  = 0;
  double        arithmetic_intensity = 0.0;  // FLOPs / total traffic bytes
};

struct TopologyAccounting {
  DemosaicNetTopologyKind kind = DemosaicNetTopologyKind::Bayer;
  int                     tile_input_h = 0;
  int                     tile_input_w = 0;
  int                     tile_output_h = 0;
  int                     tile_output_w = 0;
  int                     batch         = 1;
  std::vector<OpAccounting> ops;
  std::int64_t total_conv_flops     = 0;  // Conv2d + ConvTranspose only
  std::int64_t total_all_flops      = 0;  // + structural elemwise
  std::int64_t total_bytes_read     = 0;
  std::int64_t total_bytes_written  = 0;
  std::int64_t total_bytes_traffic  = 0;
  double       arithmetic_intensity = 0.0;
};

// Full-frame scale of one fixed-shape tile graph (product reflect-pads edge
// tiles to the same input size, so every job pays the full tile FLOP budget).
// Dimensions are the *aligned CFA cover* that BuildTileJobs schedules over
// (product neural path), not the LibRaw visible crop alone.
struct FullFrameWorkEstimate {
  TopologyAccounting per_tile;
  int                tile_inner     = 0;   // owned/export width (historical square edge field)
  int                tile_owned_w   = 0;   // owned export width (P4-C rectangles/strips)
  int                tile_owned_h   = 0;   // owned export height
  int                tile_step      = 0;   // X step (historical); prefer tile_step_x/y
  int                tile_step_x    = 0;
  int                tile_step_y    = 0;
  int                virtual_pad    = 0;   // Bayer 32 / X-Trans 12
  int                source_border  = 0;   // Bayer 31 / X-Trans 12 (tile-local)
  int                cover_width    = 0;   // aligned CFA / assembled RGB size
  int                cover_height   = 0;
  int                active_width   = 0;   // alias of cover_* (JSON compatibility)
  int                active_height  = 0;
  int                tiles_x        = 0;
  int                tiles_y        = 0;
  int                tile_count     = 0;
  int                overlap_x      = 0;   // max(0, output - step)  (X-Trans: 4)
  int                overlap_y      = 0;
  int                first_model_out_x = 0;  // Bayer: -1; X-Trans: 0
  int                first_model_out_y = 0;
  std::int64_t       full_conv_flops    = 0;
  std::int64_t       full_all_flops     = 0;
  std::int64_t       full_bytes_read    = 0;
  std::int64_t       full_bytes_written = 0;
  std::int64_t       full_bytes_traffic = 0;
  // Unique cover output pixels vs work paid (halo + inter-tile overlap).
  double             active_output_megapixels = 0.0;
  double             paid_tile_output_megapixels = 0.0;  // tile_count * owned_w * owned_h
  double             halo_work_factor = 1.0;  // paid / cover ( >= 1 )
};

// Device capability envelope used as the Phase 8.2 decision reference.
struct DeviceComputeEnvelope {
  std::string name;
  int         compute_major = 0;
  int         compute_minor = 0;
  int         multi_processor_count = 0;
  int         clock_rate_khz = 0;
  int         fp32_cores_per_sm = 0;
  // Marketing / architectural peaks (dense, FMA counted as 2 FLOPs).
  double peak_fp32_tflops = 0.0;
  double peak_tf32_tflops = 0.0;   // Tensor Core TF32 (Ampere+); 0 if N/A
  double peak_fp16_tc_tflops = 0.0;
  // Sustained fractions for *custom* kernels (not cuBLAS peak). Conservative.
  double sustained_fp32_tflops = 0.0;
  double sustained_tf32_tflops = 0.0;
  double sustained_fp16_tc_tflops = 0.0;
  std::string notes;
};

struct RooflineReport {
  FullFrameWorkEstimate work;
  DeviceComputeEnvelope device;

  // Measured intervals (ms); 0 means unavailable.
  double neural_median_ms = 0.0;
  double legacy_median_ms = 0.0;
  double stretch_target_ms = 100.0;

  // Effective rates from measured Neural interval and full-frame work.
  double effective_tflops      = 0.0;  // full_conv_flops / neural_time
  double effective_bandwidth_gbs = 0.0;

  // Work rate required to finish the *same* full-frame FLOP budget in target time.
  double required_tflops_for_legacy = 0.0;
  double required_tflops_for_stretch = 0.0;

  // Best-case wall times if the full FLOP budget ran at sustained rates (ms).
  double best_case_fp32_ms = 0.0;
  double best_case_tf32_ms = 0.0;
  double best_case_fp16_tc_ms = 0.0;

  // Utilization vs peaks (0–1+).
  double util_vs_peak_fp32      = 0.0;
  double util_vs_sustained_fp32 = 0.0;

  RooflineTrackDecision decision = RooflineTrackDecision::InsufficientTimingData;
  // Secondary: even when topology/distill is required for Legacy/100ms, FP32 kernel
  // work may still be worthwhile if measured utilization is low.
  bool        interim_fp32_kernel_headroom = false;
  std::string decision_rationale;
};

// Valid-pad Conv2d output size: floor((in - k) / s) + 1 (matches cuda::nn helpers).
[[nodiscard]] auto Conv2dValidOut(int in, int k, int s) -> int;

// FLOPs = 2 * N * Cout * Hout * Wout * Cin * kH * kW   (standard MAC=2)
// For grouped conv, Cin is channels-per-group * groups in the usual NCHW layout
// formula when cin is the full input channel count and groups divide cin/cout:
//   FLOPs = 2 * N * Cout * Hout * Wout * (Cin/groups) * kH * kW
[[nodiscard]] auto Conv2dFlops(int n, int cin, int cout, int out_h, int out_w, int k_h, int k_w,
                               int groups = 1) -> std::int64_t;

// ConvTranspose dense MAC count (groups-aware):
//   FLOPs = 2 * N * Cin * Hin * Win * (Cout/groups) * kH * kW
[[nodiscard]] auto ConvTranspose2dFlops(int n, int cin, int cout, int in_h, int in_w, int k_h,
                                        int k_w, int groups = 1) -> std::int64_t;

// Algorithmic FP32 traffic model (no cache reuse): read activations+weights+bias,
// write activations.
[[nodiscard]] auto AccountConv2dOp(const ConvOpSpec& spec) -> OpAccounting;
[[nodiscard]] auto AccountConvTransposeOp(const ConvOpSpec& spec) -> OpAccounting;
[[nodiscard]] auto AccountElemwiseMul(std::string_view name, int n, int c, int h, int w)
    -> OpAccounting;

// Exact layer list for one tile input size (product fixed shape after reflect-pad).
[[nodiscard]] auto BuildBayerTileAccounting(int tile_output, int batch = 1) -> TopologyAccounting;
[[nodiscard]] auto BuildBayerTileAccounting(int owned_w, int owned_h, int batch)
    -> TopologyAccounting;
[[nodiscard]] auto BuildXTransTileAccounting(int tile_output, int batch = 1) -> TopologyAccounting;
[[nodiscard]] auto BuildXTransTileAccounting(int owned_w, int owned_h, int batch)
    -> TopologyAccounting;
[[nodiscard]] auto BuildTileAccounting(DemosaicNetTopologyKind kind, int tile_output,
                                       int batch = 1) -> TopologyAccounting;
[[nodiscard]] auto BuildTileAccounting(DemosaicNetTopologyKind kind, int owned_w, int owned_h,
                                       int batch = 1) -> TopologyAccounting;

// Product student grid: Bayer pad32/border31, X-Trans pad12/border12; step from owned axes.
// `cover_w/h` is the aligned CFA size scheduled by BuildTileJobs. Every job pays full
// fixed-shape tile FLOPs (edge tiles are virtually reflect-padded to the policy input).
// Optional explicit tile_count overrides the product planner count (tests/harness).
// Square convenience (no override):
[[nodiscard]] auto EstimateFullFrameWork(DemosaicNetTopologyKind kind, int cover_w, int cover_h,
                                         int tile_inner = 1024) -> FullFrameWorkEstimate;
// Rectangular / strip (pass tile_count_override=-1 to use the planner count):
[[nodiscard]] auto EstimateFullFrameWork(DemosaicNetTopologyKind kind, int cover_w, int cover_h,
                                         int owned_w, int owned_h, int tile_count_override)
    -> FullFrameWorkEstimate;

[[nodiscard]] auto EstimateDeviceComputeEnvelope(const DeviceInfo& info) -> DeviceComputeEnvelope;

// Build the Phase 8.2 gate report. Pass 0 for unknown timings.
[[nodiscard]] auto BuildRooflineReport(const FullFrameWorkEstimate& work,
                                       const DeviceComputeEnvelope& device,
                                       double neural_median_ms, double legacy_median_ms,
                                       double stretch_target_ms = 100.0) -> RooflineReport;

void AppendJsonRooflineReport(std::string& out, std::string_view key, const RooflineReport& report,
                              bool trailing_comma = true);
void PrintRooflineReport(const RooflineReport& report);

}  // namespace alcedo::perf
