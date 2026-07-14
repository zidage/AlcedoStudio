//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
// One-shot OpenCL Neural full-frame timing (development report).
// Cold path includes first program compile / model load; hot mean excludes that.
//
//   OpenClDemosaicNetFullFrameTiming.exe [--warmup 1] [--iterations 3]

#include <libraw/libraw.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "decoders/processor/nn/opencl_demosaicnet_cache.hpp"
#include "decoders/processor/raw_processor.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

[[nodiscard]] auto ElapsedMs(const Clock::time_point start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

[[nodiscard]] auto Mean(const std::vector<double>& samples) -> double {
  if (samples.empty()) {
    return 0.0;
  }
  return std::accumulate(samples.begin(), samples.end(), 0.0) /
         static_cast<double>(samples.size());
}

[[nodiscard]] auto BayerPath() -> fs::path {
  return fs::path(TEST_IMG_PATH) / "raw" / "camera" / "nikon" / "d800e" /
         "Nikon-D800e-raw-00002.nef";
}

[[nodiscard]] auto XTransPath() -> fs::path {
  return fs::path(TEST_IMG_PATH) / "raw" / "camera" / "fuji" / "xt5" / "DSCF2074.RAF";
}

struct RunStats {
  std::string name;
  int         width  = 0;
  int         height = 0;
  int         out_w  = 0;
  int         out_h  = 0;
  double      cold_ms = 0.0;
  std::vector<double> hot_ms;
  bool        used_neural = false;
  std::string backend;
};

[[nodiscard]] auto ProcessOnce(LibRaw& raw, const RawDemosaicMethod method) -> ImageBuffer {
  RawParams params;
  params.gpu_backend_            = RawGpuBackend::OpenCL;
  params.demosaic_method_        = method;
  params.highlights_reconstruct_ = false;
  params.decode_res_             = DecodeRes::FULL;
  RawRuntimeColorContext context;
  const ushort           no_crop[4] = {};
  RawProcessor processor(params, raw.imgdata.rawdata, raw, context, no_crop);
  return processor.Process();
}

auto TimeFixture(const char* name, const fs::path& path, const RawCfaKind expected_kind,
                 const OpenClDemosaicNetVariant variant, const int warmup, const int iterations)
    -> RunStats {
  if (!fs::exists(path)) {
    throw std::runtime_error(std::string("fixture missing: ") + path.string());
  }

  auto raw = std::make_unique<LibRaw>();
  if (raw->open_file(path.string().c_str()) != LIBRAW_SUCCESS) {
    throw std::runtime_error("LibRaw open failed: " + path.string());
  }
  if (raw->unpack() != LIBRAW_SUCCESS) {
    throw std::runtime_error("LibRaw unpack failed: " + path.string());
  }

  const RawCfaPattern pattern = ReadLibRawCfaPattern(*raw);
  if (pattern.kind != expected_kind) {
    throw std::runtime_error(std::string(name) + ": unexpected CFA kind");
  }

  RunStats stats;
  stats.name   = name;
  stats.width  = raw->imgdata.sizes.raw_width;
  stats.height = raw->imgdata.sizes.raw_height;

  auto& cache = OpenClDemosaicNetModelCache::Instance();
  cache.Unload(variant);

  // Cold: first Neural process (may compile programs + load weights).
  {
    const auto t0  = Clock::now();
    auto       out = ProcessOnce(*raw, RawDemosaicMethod::NeuralEngine);
    stats.cold_ms  = ElapsedMs(t0);
    if (!out.gpu_data_valid_ || out.GetGPUBackend() != GpuBackendKind::OpenCL) {
      throw std::runtime_error(std::string(name) + ": cold output not on OpenCL");
    }
    stats.out_w  = out.GetOpenClImage().Width();
    stats.out_h  = out.GetOpenClImage().Height();
    stats.backend = "OpenCL";
    stats.used_neural = cache.IsLoaded(variant);
  }

  // Warm-up after cold (programs/weights resident).
  for (int i = 0; i < warmup; ++i) {
    (void)ProcessOnce(*raw, RawDemosaicMethod::NeuralEngine);
  }

  // Hot measured runs.
  stats.hot_ms.reserve(static_cast<std::size_t>(iterations));
  for (int i = 0; i < iterations; ++i) {
    const auto t0  = Clock::now();
    auto       out = ProcessOnce(*raw, RawDemosaicMethod::NeuralEngine);
    const auto ms  = ElapsedMs(t0);
    if (!out.gpu_data_valid_ || out.GetGPUBackend() != GpuBackendKind::OpenCL) {
      throw std::runtime_error(std::string(name) + ": hot output not on OpenCL");
    }
    stats.hot_ms.push_back(ms);
  }

  raw->recycle();
  return stats;
}

void PrintStats(const RunStats& s) {
  std::cout << "\n=== " << s.name << " ===\n";
  std::cout << "  CFA input:     " << s.width << " x " << s.height << "\n";
  std::cout << "  Output RGBA:   " << s.out_w << " x " << s.out_h << "\n";
  std::cout << "  Backend:       " << s.backend << "\n";
  std::cout << "  Neural cache:  " << (s.used_neural ? "loaded" : "not loaded (Legacy?)") << "\n";
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "  Cold (incl. compile/load): " << s.cold_ms << " ms\n";
  std::cout << "  Hot runs (excl. compile):  ";
  for (std::size_t i = 0; i < s.hot_ms.size(); ++i) {
    if (i != 0) {
      std::cout << ", ";
    }
    std::cout << s.hot_ms[i] << " ms";
  }
  std::cout << "\n";
  std::cout << "  Hot mean of " << s.hot_ms.size() << ":         " << Mean(s.hot_ms) << " ms\n";
}

}  // namespace
}  // namespace alcedo

auto main(int argc, char** argv) -> int {
  using namespace alcedo;

  int warmup     = 1;
  int iterations = 3;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if ((arg == "--warmup" || arg == "-w") && i + 1 < argc) {
      warmup = std::stoi(argv[++i]);
    } else if ((arg == "--iterations" || arg == "-n") && i + 1 < argc) {
      iterations = std::stoi(argv[++i]);
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: OpenClDemosaicNetFullFrameTiming [--warmup N] [--iterations N]\n";
      return 0;
    }
  }
  if (warmup < 0 || iterations < 1) {
    std::cerr << "invalid warmup/iterations\n";
    return 2;
  }

  if (!TryPrepareOpenClRuntime()) {
    auto& ctx = OpenClContext::Instance();
    if (!ctx.IsInitialized() && !ctx.TryInitialize()) {
      std::cerr << "OpenCL unavailable: " << ctx.LastInitializationError() << "\n";
      return 1;
    }
  }

  auto& ctx = OpenClContext::Instance();
  if (!ctx.IsInitialized()) {
    ctx.Initialize();
  }
  std::cout << "OpenCL device: " << ctx.Capabilities().name << "\n";
  std::cout << "OpenCL vendor: " << ctx.Capabilities().vendor << "\n";
  std::cout << "Build type:    "
#if defined(NDEBUG)
            << "Release-like (NDEBUG)\n";
#else
            << "Debug (NDEBUG not set)\n";
#endif
  std::cout << "Warmup: " << warmup << "  measured iterations: " << iterations << "\n";
  std::cout << "Timing wall-clock RawProcessor::Process with OpenCL + NeuralEngine.\n";
  std::cout << "Cold includes first program compile and model load; hot mean excludes that.\n";

  try {
    const auto bayer = TimeFixture("Nikon D800E Bayer (full frame, OpenCL Neural)", BayerPath(),
                                   RawCfaKind::Bayer2x2, OpenClDemosaicNetVariant::Bayer, warmup,
                                   iterations);
    PrintStats(bayer);

    const auto xtrans = TimeFixture("Fujifilm X-T5 X-Trans (full frame, OpenCL Neural)",
                                    XTransPath(), RawCfaKind::XTrans6x6,
                                    OpenClDemosaicNetVariant::XTrans, warmup, iterations);
    PrintStats(xtrans);

    std::cout << "\n--- Summary (hot mean, ms; compile excluded) ---\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Bayer:  " << Mean(bayer.hot_ms) << " ms\n";
    std::cout << "  X-Trans:" << Mean(xtrans.hot_ms) << " ms\n";
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
