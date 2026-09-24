//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <cstddef>
#include <cstdint>
#include <string>

#include <opencv2/core/types.hpp>

#include "decoders/processor/nn/metal_demosaicnet_cache.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"
#include "image/metal_image.hpp"

namespace alcedo::metal {

// Product-path Metal Neural Engine entry (RAW domain). Hard-fail only: every failure
// throws std::runtime_error with stage= and variant=. Never falls back to Legacy,
// CUDA, OpenCL, CPU, or another Neural implementation. On failure the caller's CFA
// texture is left unmodified and output_rgba is not published as a successful result.

enum class NeuralInjectedFailure {
  None,
  Prepare,
  Load,
  Compile,
  TileInput,
  GraphEncode,
  GraphExecute,
  TileOutput,
};

struct NeuralDemosaicOptions {
  // Null uses the process-wide lazy cache.
  MetalDemosaicNetModelCache* model_cache       = nullptr;
  MetalDemosaicNetLoadOptions load_options      = {};
  NeuralInjectedFailure       injected_failure  = NeuralInjectedFailure::None;
};

struct NeuralDemosaicResult {
  int         phase_shift_x  = 0;
  int         phase_shift_y  = 0;
  int         aligned_width  = 0;
  int         aligned_height = 0;
  std::size_t tile_count     = 0;
};

struct NeuralDemosaicTelemetry {
  double        load_ms              = 0.0;
  double        output_allocation_ms = 0.0;
  double        tiled_execution_ms   = 0.0;
  double        total_ms             = 0.0;
  std::size_t   tile_count           = 0;
  std::size_t   graph_invocation_count = 0;
  std::size_t   padded_tile_count      = 0;
  int           batch_size            = 0;
  std::uint64_t host_wait_count      = 0;
};

// Student-tiled Metal MPSGraph Neural demosaic with crop-sized assembly.
//
// Input:  R32FLOAT linear CFA after ToLinearRef (and optional ClampTexture). Never modified.
// Output: on success, replaces output_rgba with an RGBA32FLOAT texture of size
//         product_crop.width × product_crop.height. Alpha is 1. Values are linear
//         camera RGB after in-network gamma decode.
// Geometry: phase shifts and aligned size describe the aligned lattice in original CFA
//           coordinates. product_crop is in aligned-lattice coordinates; texture (0,0)
//           maps to the crop origin.
//
// Throws std::runtime_error on any failure. Stage names:
//   prepare | load | compile | tile_input | graph_encode | graph_execute | tile_output
[[nodiscard]] auto DemosaicWithNeuralEngine(const MetalImage& linear_cfa,
                                            const RawCfaPattern& camera_pattern,
                                            int phase_shift_x, int phase_shift_y,
                                            int aligned_width, int aligned_height,
                                            const cv::Rect& product_crop, MetalImage& output_rgba,
                                            const NeuralDemosaicOptions& options = {})
    -> NeuralDemosaicResult;

// Test/observability counters (product path only).
void ResetMetalNeuralPathCountersForTest();
[[nodiscard]] auto MetalNeuralSuccessCountForTest() noexcept -> std::uint64_t;
[[nodiscard]] auto MetalNeuralHostWaitCountForTest() noexcept -> std::uint64_t;
// Always zero on the product path (Metal never soft-fails to Legacy). Present so tests can
// assert the hard-fail contract explicitly.
[[nodiscard]] auto MetalNeuralLegacyFallbackCountForTest() noexcept -> std::uint64_t;
[[nodiscard]] auto LastMetalNeuralTelemetryForTest() -> NeuralDemosaicTelemetry;

}  // namespace alcedo::metal

#endif  // HAVE_METAL
