//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <libraw/libraw.h>
#include <sys/utsname.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

// OpenCV must be included before any Apple/Metal header that defines YES/NO.
#include <opencv2/core.hpp>

#include "decoders/processor/raw_processor.hpp"
#include "decoders/processor/nn/metal_demosaicnet_cache.hpp"
#include "decoders/processor/operators/gpu/metal_demosaicnet.hpp"
#include "metal/metal_context.hpp"

#include <alcedo/metal/Metal.hpp>

namespace alcedo {
namespace {

namespace fs = std::filesystem;
using Clock  = std::chrono::steady_clock;

struct Sample {
  double                         process_ms = 0.0;
  metal::NeuralDemosaicTelemetry neural;
};

struct FixtureResult {
  std::string                    name;
  std::string                    variant;
  fs::path                       path;
  int                            width           = 0;
  int                            height          = 0;
  double                         cold_process_ms = 0.0;
  metal::NeuralDemosaicTelemetry cold_neural;
  double                         cold_parse_ms   = 0.0;
  double                         cold_compile_ms = 0.0;
  std::vector<Sample>            hot;
  std::size_t                    allocated_before        = 0;
  std::size_t                    allocated_after_compile = 0;
  std::size_t                    allocated_after_hot     = 0;
  std::size_t                    owned_buffer_bytes      = 0;
  std::uint64_t                  parse_count             = 0;
  std::uint64_t                  compile_count           = 0;
  std::uint64_t                  io_allocation_count     = 0;
};

[[nodiscard]] auto Mean(const std::vector<double>& values) -> double {
  if (values.empty()) {
    return 0.0;
  }
  return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

[[nodiscard]] auto CurrentAllocatedSize() -> std::size_t {
  auto* device = MetalContext::Instance().Device();
  return device != nullptr ? static_cast<std::size_t>(device->currentAllocatedSize()) : 0;
}

[[nodiscard]] auto ProcessOnce(LibRaw& raw) -> Sample {
  RawParams params;
  params.gpu_backend_            = RawGpuBackend::Metal;
  params.demosaic_method_        = RawDemosaicMethod::NeuralEngine;
  params.highlights_reconstruct_ = false;
  params.decode_res_             = DecodeRes::FULL;
  RawRuntimeColorContext context;
  const ushort           no_crop[4] = {};

  const auto             start      = Clock::now();
  RawProcessor           processor(params, raw.imgdata.rawdata, raw, context, no_crop);
  ImageBuffer            output = processor.Process();
  const double process_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  if (!output.gpu_data_valid_ || output.GetGPUBackend() != GpuBackendKind::Metal) {
    throw std::runtime_error("Metal Neural run did not produce a Metal GPU image");
  }
  return {process_ms, metal::LastMetalNeuralTelemetryForTest()};
}

[[nodiscard]] auto RunFixture(const std::string& name, const std::string& variant,
                              const fs::path& path, MetalDemosaicNetVariant cache_variant,
                              int iterations) -> FixtureResult {
  if (!fs::is_regular_file(path)) {
    throw std::runtime_error("local fixture missing: " + path.string());
  }

  auto raw = std::make_unique<LibRaw>();
  if (raw->open_file(path.string().c_str()) != LIBRAW_SUCCESS || raw->unpack() != LIBRAW_SUCCESS) {
    throw std::runtime_error("LibRaw failed to open/unpack: " + path.string());
  }

  auto& cache = MetalDemosaicNetModelCache::Instance();
  cache.Unload(cache_variant);
  metal::ResetMetalNeuralPathCountersForTest();

  FixtureResult result;
  result.name                    = name;
  result.variant                 = variant;
  result.path                    = path;
  result.width                   = raw->imgdata.sizes.raw_width;
  result.height                  = raw->imgdata.sizes.raw_height;
  result.allocated_before        = CurrentAllocatedSize();

  const Sample cold              = ProcessOnce(*raw);
  result.cold_process_ms         = cold.process_ms;
  result.cold_neural             = cold.neural;
  result.cold_parse_ms           = cache.last_parse_ms();
  result.cold_compile_ms         = cache.last_compile_ms();
  result.allocated_after_compile = CurrentAllocatedSize();

  (void)ProcessOnce(*raw);  // untimed warm-up with a ready cache
  for (int i = 0; i < iterations; ++i) {
    result.hot.push_back(ProcessOnce(*raw));
  }

  result.allocated_after_hot = CurrentAllocatedSize();
  result.owned_buffer_bytes  = cache.OwnedBufferBytes();
  result.parse_count         = cache.parse_count();
  result.compile_count       = cache.compile_count();
  result.io_allocation_count = cache.input_output_allocation_count();
  raw->recycle();
  return result;
}

[[nodiscard]] auto NeuralMean(const FixtureResult& result) -> double {
  std::vector<double> values;
  for (const auto& sample : result.hot) {
    values.push_back(sample.neural.tiled_execution_ms);
  }
  return Mean(values);
}

void PrintResult(const FixtureResult& result) {
  std::cout << "\n"
            << result.name << " (" << result.width << "x" << result.height << ", "
            << result.cold_neural.tile_count << " tiles)\n"
            << "  cold: parse=" << result.cold_parse_ms << " ms compile=" << result.cold_compile_ms
            << " ms first-neural=" << result.cold_neural.tiled_execution_ms
            << " ms process=" << result.cold_process_ms << " ms\n";
  std::vector<double> process_values;
  for (std::size_t i = 0; i < result.hot.size(); ++i) {
    const auto& sample = result.hot[i];
    process_values.push_back(sample.process_ms);
    std::cout << "  hot[" << i << "]: neural=" << sample.neural.tiled_execution_ms
              << " ms process=" << sample.process_ms
              << " ms waits=" << sample.neural.host_wait_count
              << " graph_invocations=" << sample.neural.graph_invocation_count << "\n";
  }
  std::cout << "  mean: neural=" << NeuralMean(result) << " ms process=" << Mean(process_values)
            << " ms target=" << (NeuralMean(result) < 500.0 ? "PASS" : "FAIL") << "\n"
            << "  memory: before=" << result.allocated_before
            << " after_compile=" << result.allocated_after_compile
            << " after_hot=" << result.allocated_after_hot
            << " owned_buffers=" << result.owned_buffer_bytes << " bytes\n";
}

void WriteJson(const fs::path& path, const std::string& device_name,
               const std::vector<FixtureResult>& results) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("cannot write JSON: " + path.string());
  }
  struct utsname os{};
  uname(&os);
  out << std::fixed << std::setprecision(3);
  out << "{\n  \"device\": \"" << device_name << "\",\n"
      << "  \"os\": \"" << os.sysname << " " << os.release << " " << os.machine << "\",\n"
      << "  \"fixtures\": [\n";
  for (std::size_t i = 0; i < results.size(); ++i) {
    const auto& r = results[i];
    out << "    {\n"
        << "      \"name\": \"" << r.name << "\",\n"
        << "      \"variant\": \"" << r.variant << "\",\n"
        << "      \"width\": " << r.width << ", \"height\": " << r.height << ",\n"
        << "      \"tile_count\": " << r.cold_neural.tile_count << ",\n"
        << "      \"batch_size\": " << r.cold_neural.batch_size << ",\n"
        << "      \"graph_invocation_count\": " << r.cold_neural.graph_invocation_count << ",\n"
        << "      \"padded_tile_count\": " << r.cold_neural.padded_tile_count << ",\n"
        << "      \"cold_parse_ms\": " << r.cold_parse_ms << ",\n"
        << "      \"cold_compile_ms\": " << r.cold_compile_ms << ",\n"
        << "      \"cold_first_neural_ms\": " << r.cold_neural.tiled_execution_ms << ",\n"
        << "      \"hot_neural_ms\": [";
    for (std::size_t j = 0; j < r.hot.size(); ++j) {
      if (j != 0) out << ", ";
      out << r.hot[j].neural.tiled_execution_ms;
    }
    out << "],\n      \"hot_neural_mean_ms\": " << NeuralMean(r) << ",\n"
        << "      \"under_500_ms\": " << (NeuralMean(r) < 500.0 ? "true" : "false") << ",\n"
        << "      \"allocated_before\": " << r.allocated_before << ",\n"
        << "      \"allocated_after_compile\": " << r.allocated_after_compile << ",\n"
        << "      \"allocated_after_hot\": " << r.allocated_after_hot << ",\n"
        << "      \"owned_buffer_bytes\": " << r.owned_buffer_bytes << ",\n"
        << "      \"parse_count\": " << r.parse_count << ",\n"
        << "      \"compile_count\": " << r.compile_count << ",\n"
        << "      \"io_allocation_count\": " << r.io_allocation_count << "\n"
        << "    }" << (i + 1 == results.size() ? "\n" : ",\n");
  }
  out << "  ]\n}\n";
}

}  // namespace
}  // namespace alcedo

int main(int argc, char** argv) {
#ifndef ALCEDO_METAL_DEMOSAICNET_RELEASE_BUILD
  std::cerr << "warning: authoritative Metal DemosaicNet timings require a Release build\n";
#endif
  try {
    int                   iterations = 3;
    std::string           variant    = "both";
    std::filesystem::path json_path  = "build/perf/metal_demosaicnet_m4_profile.json";
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--iterations" && i + 1 < argc) {
        iterations = std::stoi(argv[++i]);
      } else if (arg == "--json" && i + 1 < argc) {
        json_path = argv[++i];
      } else if (arg == "--variant" && i + 1 < argc) {
        variant = argv[++i];
      } else {
        throw std::runtime_error(
            "usage: MetalDemosaicNetFullFrameTiming [--iterations N] "
            "[--variant bayer|xtrans|both] [--json path]");
      }
    }
    if (variant != "bayer" && variant != "xtrans" && variant != "both") {
      throw std::runtime_error("--variant must be bayer, xtrans, or both");
    }

    auto* device = alcedo::MetalContext::Instance().Device();
    if (device == nullptr) {
      throw std::runtime_error("Metal device unavailable");
    }
    const char* device_chars = device->name() != nullptr ? device->name()->utf8String() : nullptr;
    const std::string           device_name = device_chars != nullptr ? device_chars : "unknown";
    const std::filesystem::path local       = std::filesystem::path(TEST_IMG_PATH) / "local";

    std::cout << std::fixed << std::setprecision(3) << "Metal device: " << device_name << "\n";
    std::vector<alcedo::FixtureResult> results;
    if (variant == "bayer" || variant == "both") {
      results.push_back(alcedo::RunFixture("metal_neural_bayer_s5m2.RW2", "bayer_s24_d8",
                                           local / "metal_neural_bayer_s5m2.RW2",
                                           alcedo::MetalDemosaicNetVariant::Bayer, iterations));
      alcedo::PrintResult(results.back());
    }
    if (variant == "xtrans" || variant == "both") {
      results.push_back(alcedo::RunFixture("metal_neural_xtrans_xt5.RAF", "xtrans_p2_s32_d4",
                                           local / "metal_neural_xtrans_xt5.RAF",
                                           alcedo::MetalDemosaicNetVariant::XTrans, iterations));
      alcedo::PrintResult(results.back());
    }
    alcedo::WriteJson(json_path, device_name, results);
    std::cout << "\nJSON: " << json_path << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
