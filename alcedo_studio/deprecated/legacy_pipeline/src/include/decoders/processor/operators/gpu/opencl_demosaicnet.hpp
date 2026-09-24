//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstdint>
#include <string>

#include "decoders/processor/nn/opencl_demosaicnet_cache.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"
#include "image/opencl_image.hpp"

namespace alcedo::OpenCL {

// Product-path OpenCL Neural Engine entry (RAW domain). Soft-fail only: on failure
// `rgb_rgba` is left untouched so the caller can run OpenCL Legacy from the original
// linear CFA. Failures never switch to CUDA or CPU.

enum class OpenClNeuralInjectedFailure {
  None,
  ModelLoad,
  Enqueue,
};

struct OpenClNeuralDemosaicOptions {
  // Null uses the process-wide lazy cache. Injection keeps load-failure tests independent.
  OpenClDemosaicNetModelCache*   model_cache  = nullptr;
  OpenClDemosaicNetLoadOptions   load_options = {};
  OpenClNeuralInjectedFailure    injected_failure = OpenClNeuralInjectedFailure::None;
};

struct OpenClNeuralDemosaicResult {
  bool        succeeded      = false;
  int         phase_shift_x  = 0;
  int         phase_shift_y  = 0;
  int         aligned_width  = 0;
  int         aligned_height = 0;
  std::size_t tile_count     = 0;
  std::string error;
  std::string failure_stage;  // prepare | load | enqueue | sync
  std::string variant;        // Bayer | XTrans
};

// Clamp linear samples to [0,1] in place (HLR-off product path). Accepts any float type
// whose element count is width * height * channels for contiguous OpenClImage buffers.
void Clamp01(opencl::OpenClImage& image);

// Student-tiled OpenCL Neural demosaic.
//
// Input:  CV_32FC1 linear CFA after ToLinearRef (and optional Clamp01). Never modified.
// Output: on success, CV_32FC4 RGBA covering the full phase-aligned CFA lattice (same
//         width/height as the period-trimmed aligned mosaic). Alpha is 1. Values are
//         linear camera RGB after in-network gamma decode.
//
// Queueing: pack/forward/assemble for every tile is enqueued on the context's single
// in-order queue; this function waits once at the Neural-stage boundary.
[[nodiscard]] auto DemosaicWithNeuralEngine(const opencl::OpenClImage& linear_cfa,
                                            const RawCfaPattern& camera_pattern,
                                            opencl::OpenClImage& rgb_rgba,
                                            const OpenClNeuralDemosaicOptions& options = {})
    -> OpenClNeuralDemosaicResult;

// Test/observability counters (product path only).
void ResetOpenClNeuralPathCountersForTest();
[[nodiscard]] auto OpenClNeuralSuccessCountForTest() noexcept -> std::uint64_t;
[[nodiscard]] auto OpenClNeuralFallbackReadyCountForTest() noexcept -> std::uint64_t;
[[nodiscard]] auto OpenClNeuralHostWaitCountForTest() noexcept -> std::uint64_t;

// Called by RawProcessor when OpenCL Neural soft-fails into OpenCL Legacy.
void NoteOpenClNeuralLegacyFallbackForTest();
[[nodiscard]] auto OpenClNeuralLegacyFallbackCountForTest() noexcept -> std::uint64_t;

}  // namespace alcedo::OpenCL

#endif  // HAVE_OPENCL
