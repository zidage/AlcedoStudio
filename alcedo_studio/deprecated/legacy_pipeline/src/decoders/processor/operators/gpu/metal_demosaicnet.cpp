//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_METAL

#include "decoders/processor/operators/gpu/metal_demosaicnet.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "decoders/processor/nn/demosaicnet_specs.hpp"
#include "decoders/processor/nn/metal_demosaicnet_tiled.hpp"

namespace alcedo::metal {
namespace {

std::atomic<std::uint64_t> g_success_count{0};
std::atomic<std::uint64_t> g_host_wait_count{0};
// Intentionally never incremented: Metal Neural has no Legacy soft-fail path.
std::atomic<std::uint64_t> g_legacy_fallback_count{0};
std::mutex                 g_telemetry_mutex;
NeuralDemosaicTelemetry    g_last_telemetry;
using Clock = std::chrono::steady_clock;

[[nodiscard]] auto ElapsedMs(const Clock::time_point start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

[[nodiscard]] auto VariantArchitecture(const RawCfaKind kind) -> const char* {
  return kind == RawCfaKind::XTrans6x6 ? DemosaicNetXTransSpec::kArchitecture
                                       : DemosaicNetBayerSpec::kArchitecture;
}

[[nodiscard]] auto VariantEnum(const RawCfaKind kind) -> MetalDemosaicNetVariant {
  return kind == RawCfaKind::XTrans6x6 ? MetalDemosaicNetVariant::XTrans
                                       : MetalDemosaicNetVariant::Bayer;
}

[[noreturn]] void ThrowStage(const char* stage, const char* variant, const std::string& detail) {
  throw std::runtime_error(std::string("Metal Neural Engine failed (stage=") + stage +
                           ", variant=" + variant + "): " + detail);
}

void InjectIfRequested(NeuralInjectedFailure injected, NeuralInjectedFailure target,
                       const char* stage, const char* variant) {
  if (injected == target) {
    ThrowStage(stage, variant, std::string("injected ") + stage + " failure");
  }
}

}  // namespace

void ResetMetalNeuralPathCountersForTest() {
  g_success_count.store(0, std::memory_order_relaxed);
  g_host_wait_count.store(0, std::memory_order_relaxed);
  g_legacy_fallback_count.store(0, std::memory_order_relaxed);
  ResetMetalDemosaicNetHostWaitCountForTest();
  std::lock_guard<std::mutex> lock(g_telemetry_mutex);
  g_last_telemetry = {};
}

auto MetalNeuralSuccessCountForTest() noexcept -> std::uint64_t {
  return g_success_count.load(std::memory_order_relaxed);
}

auto MetalNeuralHostWaitCountForTest() noexcept -> std::uint64_t {
  return g_host_wait_count.load(std::memory_order_relaxed);
}

auto MetalNeuralLegacyFallbackCountForTest() noexcept -> std::uint64_t {
  return g_legacy_fallback_count.load(std::memory_order_relaxed);
}

auto LastMetalNeuralTelemetryForTest() -> NeuralDemosaicTelemetry {
  std::lock_guard<std::mutex> lock(g_telemetry_mutex);
  return g_last_telemetry;
}

auto DemosaicWithNeuralEngine(const MetalImage& linear_cfa, const RawCfaPattern& camera_pattern,
                              int phase_shift_x, int phase_shift_y, int aligned_width,
                              int aligned_height, const cv::Rect& product_crop,
                              MetalImage& output_rgba, const NeuralDemosaicOptions& options)
    -> NeuralDemosaicResult {
  const auto total_start = Clock::now();
  const char* variant = VariantArchitecture(camera_pattern.kind);

  InjectIfRequested(options.injected_failure, NeuralInjectedFailure::Prepare, "prepare", variant);

  if (linear_cfa.Empty() || linear_cfa.Format() != PixelFormat::R32FLOAT) {
    ThrowStage("prepare", variant, "requires a non-empty R32FLOAT linear CFA");
  }
  if (camera_pattern.kind != RawCfaKind::Bayer2x2 && camera_pattern.kind != RawCfaKind::XTrans6x6) {
    ThrowStage("prepare", variant, "unsupported CFA kind for Neural demosaic");
  }
  if (aligned_width <= 0 || aligned_height <= 0) {
    ThrowStage("prepare", variant, "aligned dimensions must be positive");
  }
  if (product_crop.width <= 0 || product_crop.height <= 0) {
    ThrowStage("prepare", variant, "product crop is empty");
  }
  if (phase_shift_x < 0 || phase_shift_y < 0 ||
      phase_shift_x + aligned_width > static_cast<int>(linear_cfa.Width()) ||
      phase_shift_y + aligned_height > static_cast<int>(linear_cfa.Height())) {
    ThrowStage("prepare", variant, "phase shift + aligned size exceeds CFA texture");
  }

  InjectIfRequested(options.injected_failure, NeuralInjectedFailure::Load, "load", variant);

  MetalDemosaicNetModelCache& cache =
      options.model_cache == nullptr ? MetalDemosaicNetModelCache::Instance() : *options.model_cache;
  const MetalDemosaicNetVariant cache_variant = VariantEnum(camera_pattern.kind);
  const auto load_start = Clock::now();
  if (!cache.EnsureLoaded(cache_variant, options.load_options)) {
    ThrowStage("load", variant, cache.LastError().empty() ? "model load failed" : cache.LastError());
  }
  const double load_ms = ElapsedMs(load_start);

  InjectIfRequested(options.injected_failure, NeuralInjectedFailure::Compile, "compile", variant);

  // Allocate crop-sized output only after load/compile gates so a failed load never
  // publishes a product-visible Neural texture into the caller's slot.
  const auto alloc_start = Clock::now();
  MetalImage neural_rgba = MetalImage::Create2D(static_cast<uint32_t>(product_crop.width),
                                                static_cast<uint32_t>(product_crop.height),
                                                PixelFormat::RGBA32FLOAT);
  const double output_allocation_ms = ElapsedMs(alloc_start);

  InjectIfRequested(options.injected_failure, NeuralInjectedFailure::TileInput, "tile_input",
                    variant);
  InjectIfRequested(options.injected_failure, NeuralInjectedFailure::GraphEncode, "graph_encode",
                    variant);
  InjectIfRequested(options.injected_failure, NeuralInjectedFailure::GraphExecute, "graph_execute",
                    variant);
  InjectIfRequested(options.injected_failure, NeuralInjectedFailure::TileOutput, "tile_output",
                    variant);

  MetalDemosaicNetTiledDispatch dispatch;
  dispatch.cfa_image      = &linear_cfa;
  dispatch.output_rgba    = &neural_rgba;
  dispatch.shift_sx       = phase_shift_x;
  dispatch.shift_sy       = phase_shift_y;
  dispatch.aligned_width  = aligned_width;
  dispatch.aligned_height = aligned_height;
  dispatch.product_crop   = product_crop;
  dispatch.commit_and_wait = true;

  MetalDemosaicNetTiledExecutor executor;
  MetalDemosaicNetTiledResult   tiled;
  const auto tiled_start = Clock::now();
  try {
    if (cache_variant == MetalDemosaicNetVariant::Bayer) {
      tiled = executor.EnqueueBayer(cache.Bayer(), dispatch);
    } else {
      tiled = executor.EnqueueXTrans(cache.XTrans(), dispatch);
    }
  } catch (const std::exception& e) {
    // Preserve stage tags from the tiled layer when present; otherwise wrap with variant.
    const std::string message = e.what();
    if (message.find("stage=") != std::string::npos) {
      throw;
    }
    ThrowStage("graph_execute", variant, message);
  }
  const double tiled_execution_ms = ElapsedMs(tiled_start);

  // Publish only after the tile loop completed without error.
  output_rgba = std::move(neural_rgba);

  NeuralDemosaicResult result;
  result.phase_shift_x  = phase_shift_x;
  result.phase_shift_y  = phase_shift_y;
  result.aligned_width  = aligned_width;
  result.aligned_height = aligned_height;
  result.tile_count     = tiled.tile_count;

  g_success_count.fetch_add(1, std::memory_order_relaxed);
  if (tiled.host_wait_count > 0) {
    g_host_wait_count.fetch_add(tiled.host_wait_count, std::memory_order_relaxed);
  }
  {
    std::lock_guard<std::mutex> lock(g_telemetry_mutex);
    g_last_telemetry.load_ms              = load_ms;
    g_last_telemetry.output_allocation_ms = output_allocation_ms;
    g_last_telemetry.tiled_execution_ms   = tiled_execution_ms;
    g_last_telemetry.total_ms             = ElapsedMs(total_start);
    g_last_telemetry.tile_count           = tiled.tile_count;
    g_last_telemetry.graph_invocation_count = tiled.graph_invocation_count;
    g_last_telemetry.padded_tile_count      = tiled.padded_tile_count;
    g_last_telemetry.batch_size             = 2;
    g_last_telemetry.host_wait_count      = tiled.host_wait_count;
  }
  return result;
}

}  // namespace alcedo::metal

#endif  // HAVE_METAL
