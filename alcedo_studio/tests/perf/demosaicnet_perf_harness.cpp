//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Phase 8.1: standalone DemosaicNet / Neural Engine performance harness.
// Not a correctness suite — opt-in via ALCEDO_ENABLE_GPU_PERF_TESTS for ctest.
//
// Authoritative numbers: build Release and run:
//   build\release\alcedo_studio\tests\DemosaicNetPerfHarness.exe ^
//     --fixture bayer|xtrans|all --method legacy|neural|both --mode full|tile|conv ^
//     --warmup 3 --iterations 20 --tile-size 1024 --lanes 1 ^
//     --output build/perf/demosaicnet.json

#include <cuda_runtime.h>
#include <libraw/libraw.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>
#include <opencv2/core/cuda_stream_accessor.hpp>

#include "cuda/nn/common.hpp"
#include "cuda/nn/conv2d.hpp"
#include "cuda/nn/device_buffer.hpp"
#include "cuda/nn/tensor.hpp"
#include "cuda/nn/workspace.hpp"
#include "decoders/processor/nn/demosaicnet_bayer.hpp"
#include "decoders/processor/nn/demosaicnet_cache.hpp"
#include "decoders/processor/nn/demosaicnet_preprocess.hpp"
#include "decoders/processor/nn/demosaicnet_xtrans.hpp"
#include "decoders/processor/operators/gpu/cuda_demosaicnet.hpp"
#include "decoders/processor/raw_processor.hpp"
#include "decoders/processor/raw_processor_internal.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"
#include "demosaicnet_perf_metrics.hpp"
#include "image/image_buffer.hpp"

#ifndef ALCEDO_GIT_COMMIT
#define ALCEDO_GIT_COMMIT "unknown"
#endif

namespace alcedo {
namespace {

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
namespace perf = alcedo::perf;

enum class FixtureKind { Bayer, XTrans };
enum class MethodKind { Legacy, Neural, Both };
enum class ModeKind { Full, Tile, Conv };

struct HarnessConfig {
  FixtureKind fixture          = FixtureKind::Bayer;
  bool        run_bayer        = true;
  bool        run_xtrans       = false;
  MethodKind  method           = MethodKind::Both;
  ModeKind    mode             = ModeKind::Full;
  int         warmup           = 3;
  int         iterations       = 20;
  int         tile_size        = 1024;
  int         lanes            = 1;
  bool        profile_ranges   = false;
  fs::path    output_json;
  fs::path    fixture_override;
};

struct FixtureSpec {
  FixtureKind         kind;
  const char*         name;
  fs::path            path;
  DemosaicNetVariant  variant;
};

[[nodiscard]] auto ElapsedMs(const Clock::time_point start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

[[nodiscard]] auto DefaultBayerPath() -> fs::path {
  return fs::path(TEST_IMG_PATH) / "raw" / "camera" / "nikon" / "d800e" /
         "Nikon-D800e-raw-00002.nef";
}

[[nodiscard]] auto DefaultXTransPath() -> fs::path {
  return fs::path(TEST_IMG_PATH) / "raw" / "camera" / "fuji" / "xt5" / "DSCF2074.RAF";
}

void PrintUsage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " [options]\n"
      << "  --fixture bayer|xtrans|all     Primary fixtures (default: bayer)\n"
      << "  --method legacy|neural|both    Method under test (default: both)\n"
      << "  --mode full|tile|conv          Measurement mode (default: full)\n"
      << "  --warmup N                     Warm-up iterations (default: 3)\n"
      << "  --iterations N                 Measured iterations (default: 20)\n"
      << "  --tile-size N                  Inner tile edge for tile/conv (default: 1024)\n"
      << "  --lanes N                      Recorded lane count (default: 1; multi-lane later)\n"
      << "  --profile-ranges               Named CUDA-event ranges (tile/conv)\n"
      << "  --output path.json             Write machine-readable JSON\n"
      << "  --raw path                     Override fixture RAW path (single fixture)\n"
      << "  --help                         Show this help\n";
}

[[nodiscard]] auto ParseArgs(const int argc, char** argv) -> HarnessConfig {
  HarnessConfig cfg;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(std::string("missing value for ") + name);
      }
      return argv[++i];
    };
    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      std::exit(0);
    }
    if (arg == "--fixture") {
      const auto v = need("--fixture");
      if (v == "bayer") {
        cfg.run_bayer  = true;
        cfg.run_xtrans = false;
        cfg.fixture    = FixtureKind::Bayer;
      } else if (v == "xtrans") {
        cfg.run_bayer  = false;
        cfg.run_xtrans = true;
        cfg.fixture    = FixtureKind::XTrans;
      } else if (v == "all") {
        cfg.run_bayer  = true;
        cfg.run_xtrans = true;
      } else {
        throw std::runtime_error("unknown --fixture " + v);
      }
    } else if (arg == "--method") {
      const auto v = need("--method");
      if (v == "legacy") {
        cfg.method = MethodKind::Legacy;
      } else if (v == "neural") {
        cfg.method = MethodKind::Neural;
      } else if (v == "both") {
        cfg.method = MethodKind::Both;
      } else {
        throw std::runtime_error("unknown --method " + v);
      }
    } else if (arg == "--mode") {
      const auto v = need("--mode");
      if (v == "full") {
        cfg.mode = ModeKind::Full;
      } else if (v == "tile") {
        cfg.mode = ModeKind::Tile;
      } else if (v == "conv") {
        cfg.mode = ModeKind::Conv;
      } else {
        throw std::runtime_error("unknown --mode " + v);
      }
    } else if (arg == "--warmup") {
      cfg.warmup = std::stoi(need("--warmup"));
    } else if (arg == "--iterations") {
      cfg.iterations = std::stoi(need("--iterations"));
    } else if (arg == "--tile-size") {
      cfg.tile_size = std::stoi(need("--tile-size"));
    } else if (arg == "--lanes") {
      cfg.lanes = std::stoi(need("--lanes"));
    } else if (arg == "--profile-ranges") {
      cfg.profile_ranges = true;
    } else if (arg == "--output") {
      cfg.output_json = need("--output");
    } else if (arg == "--raw") {
      cfg.fixture_override = need("--raw");
    } else {
      throw std::runtime_error("unknown argument " + std::string(arg));
    }
  }
  if (cfg.warmup < 0 || cfg.iterations < 1) {
    throw std::runtime_error("--warmup must be >= 0 and --iterations must be >= 1");
  }
  if (cfg.tile_size < 64 || cfg.lanes < 1) {
    throw std::runtime_error("--tile-size must be >= 64 and --lanes must be >= 1");
  }
  return cfg;
}

[[nodiscard]] auto EnsureCuda() -> void {
  int count = 0;
  if (cudaGetDeviceCount(&count) != cudaSuccess || count <= 0) {
    throw std::runtime_error("No CUDA device available");
  }
  cuda::nn::CheckCuda(cudaSetDevice(0), "cudaSetDevice");
  cv::cuda::setDevice(0);
}

[[nodiscard]] auto OpenUnpackedRaw(const fs::path& path) -> std::unique_ptr<LibRaw> {
  if (!fs::exists(path)) {
    throw std::runtime_error("RAW fixture not found: " + path.string());
  }
  auto raw = std::make_unique<LibRaw>();
  if (raw->open_file(path.string().c_str()) != LIBRAW_SUCCESS) {
    throw std::runtime_error("LibRaw::open_file failed: " + path.string());
  }
  if (raw->unpack() != LIBRAW_SUCCESS) {
    throw std::runtime_error("LibRaw::unpack failed: " + path.string());
  }
  if (raw->imgdata.rawdata.raw_image == nullptr) {
    throw std::runtime_error("LibRaw has no raw_image for " + path.string());
  }
  return raw;
}

[[nodiscard]] auto ValidateRgbaOutput(const ImageBuffer& output, const char* label) -> cv::Size {
  if (!output.gpu_data_valid_ || output.GetGPUBackend() != GpuBackendKind::CUDA) {
    throw std::runtime_error(std::string(label) + ": expected CUDA GpuMat output");
  }
  const auto& gpu = output.GetCUDAImage();
  if (gpu.empty() || gpu.type() != CV_32FC4) {
    throw std::runtime_error(std::string(label) + ": expected non-empty CV_32FC4");
  }
  cv::Mat host;
  gpu.download(host);
  for (int y = 0; y < host.rows; y += std::max(1, host.rows / 32)) {
    for (int x = 0; x < host.cols; x += std::max(1, host.cols / 32)) {
      const cv::Vec4f p = host.at<cv::Vec4f>(y, x);
      for (int c = 0; c < 4; ++c) {
        if (!std::isfinite(p[c])) {
          throw std::runtime_error(std::string(label) + ": non-finite sample");
        }
      }
    }
  }
  return host.size();
}

[[nodiscard]] auto RunFullProcess(LibRaw& raw, const RawDemosaicMethod method) -> ImageBuffer {
  RawParams params;
  params.gpu_backend_            = RawGpuBackend::CUDA;
  params.demosaic_method_        = method;
  params.highlights_reconstruct_ = false;
  params.decode_res_             = DecodeRes::FULL;
  params.use_camera_wb_          = true;
  RawRuntimeColorContext context;
  const ushort           no_crop[4] = {};
  RawProcessor           processor(params, raw.imgdata.rawdata, raw, context, no_crop);
  return processor.Process();
}

struct FullFixtureResult {
  std::string   fixture_name;
  std::string   fixture_path;
  int           raw_width           = 0;
  int           raw_height          = 0;
  int           active_width        = 0;
  int           active_height       = 0;
  double        active_megapixels   = 0.0;
  int           tile_count          = 0;
  int           tile_size           = 0;
  int           lanes               = 1;
  double           cold_load_ms        = 0.0;
  perf::TimingStats legacy_stats;
  perf::TimingStats neural_stats;
  cv::Size         legacy_out_size;
  cv::Size         neural_out_size;
  std::uint64_t allocation_gen_start = 0;
  std::uint64_t allocation_gen_end   = 0;
  bool          allocation_stable    = true;
  std::size_t   free_bytes_after     = 0;
  std::size_t   total_bytes_after    = 0;
};

[[nodiscard]] auto EstimateTileCount(const int active_w, const int active_h, const int tile_inner)
    -> int {
  if (active_w <= 0 || active_h <= 0 || tile_inner <= 0) {
    return 0;
  }
  const int tiles_x = (active_w + tile_inner - 1) / tile_inner;
  const int tiles_y = (active_h + tile_inner - 1) / tile_inner;
  return tiles_x * tiles_y;
}

auto RunFullMode(const FixtureSpec& fixture, const HarnessConfig& cfg) -> FullFixtureResult {
  FullFixtureResult result;
  result.fixture_name = fixture.name;
  result.fixture_path = fixture.path.string();
  result.tile_size    = cfg.tile_size;
  result.lanes        = cfg.lanes;

  auto raw              = OpenUnpackedRaw(fixture.path);
  result.raw_width      = raw->imgdata.sizes.raw_width;
  result.raw_height     = raw->imgdata.sizes.raw_height;
  result.active_width   = raw->imgdata.sizes.width;
  result.active_height  = raw->imgdata.sizes.height;
  result.active_megapixels =
      (static_cast<double>(result.active_width) * static_cast<double>(result.active_height)) /
      1.0e6;
  result.tile_count =
      EstimateTileCount(result.active_width, result.active_height, cfg.tile_size);

  const bool want_legacy = cfg.method == MethodKind::Legacy || cfg.method == MethodKind::Both;
  const bool want_neural = cfg.method == MethodKind::Neural || cfg.method == MethodKind::Both;

  // Correctness pass before timing.
  if (want_legacy) {
    result.legacy_out_size =
        ValidateRgbaOutput(RunFullProcess(*raw, RawDemosaicMethod::Legacy), "legacy correctness");
  }
  if (want_neural) {
    auto& cache = DemosaicNetModelCache::Instance();
    cache.Unload(fixture.variant);
    const auto cold_start = Clock::now();
    if (!cache.EnsureLoaded(fixture.variant)) {
      throw std::runtime_error("cold model load failed: " + cache.LastError());
    }
    result.cold_load_ms = ElapsedMs(cold_start);
    result.neural_out_size =
        ValidateRgbaOutput(RunFullProcess(*raw, RawDemosaicMethod::NeuralEngine),
                           "neural correctness");
  }

  // Warm-up (models, CUDA context, workspaces).
  for (int i = 0; i < cfg.warmup; ++i) {
    if (want_legacy) {
      (void)RunFullProcess(*raw, RawDemosaicMethod::Legacy);
    }
    if (want_neural) {
      (void)RunFullProcess(*raw, RawDemosaicMethod::NeuralEngine);
    }
  }
  cuda::nn::CheckCuda(cudaDeviceSynchronize(), "full warm-up sync");

  std::vector<double> legacy_samples;
  std::vector<double> neural_samples;
  legacy_samples.reserve(static_cast<std::size_t>(cfg.iterations));
  neural_samples.reserve(static_cast<std::size_t>(cfg.iterations));

  // Alternate method order per iteration to reduce clock/temperature bias.
  for (int i = 0; i < cfg.iterations; ++i) {
    const bool legacy_first = (i % 2) == 0;
    auto run_legacy = [&] {
      const auto t0 = Clock::now();
      auto out      = RunFullProcess(*raw, RawDemosaicMethod::Legacy);
      cuda::nn::CheckCuda(cudaDeviceSynchronize(), "legacy full sync");
      const double ms = ElapsedMs(t0);
      (void)ValidateRgbaOutput(out, "legacy timed");
      legacy_samples.push_back(ms);
    };
    auto run_neural = [&] {
      const auto t0 = Clock::now();
      auto out      = RunFullProcess(*raw, RawDemosaicMethod::NeuralEngine);
      cuda::nn::CheckCuda(cudaDeviceSynchronize(), "neural full sync");
      const double ms = ElapsedMs(t0);
      (void)ValidateRgbaOutput(out, "neural timed");
      neural_samples.push_back(ms);
    };

    if (legacy_first) {
      if (want_legacy) {
        run_legacy();
      }
      if (want_neural) {
        run_neural();
      }
    } else {
      if (want_neural) {
        run_neural();
      }
      if (want_legacy) {
        run_legacy();
      }
    }
  }

  result.legacy_stats = perf::ComputeTimingStats(std::move(legacy_samples));
  result.neural_stats = perf::ComputeTimingStats(std::move(neural_samples));

  size_t free_b = 0;
  size_t total_b = 0;
  cudaMemGetInfo(&free_b, &total_b);
  result.free_bytes_after  = free_b;
  result.total_bytes_after = total_b;
  return result;
}

struct TileFixtureResult {
  std::string   fixture_name;
  int           input_size          = 0;
  int           output_size         = 0;
  double            cold_load_ms         = 0.0;
  perf::TimingStats wall_stats;
  perf::TimingStats cuda_stats;
  std::uint64_t     allocation_gen_start = 0;
  std::uint64_t allocation_gen_end   = 0;
  bool          allocation_stable    = true;
  std::string   profile_json;  // optional named ranges object body
};

[[nodiscard]] auto MakeSyntheticCfa(const int size, const unsigned seed) -> cv::Mat {
  cv::Mat cfa(size, size, CV_32FC1);
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> dist(0.0F, 1.0F);
  for (int y = 0; y < size; ++y) {
    float* row = cfa.ptr<float>(y);
    for (int x = 0; x < size; ++x) {
      row[x] = dist(rng);
    }
  }
  return cfa;
}

auto RunTileMode(const FixtureSpec& fixture, const HarnessConfig& cfg) -> TileFixtureResult {
  TileFixtureResult result;
  result.fixture_name = fixture.name;

  const int border =
      fixture.kind == FixtureKind::Bayer ? BayerDemosaicNet::kSpatialLoss / 2
                                         : XTransDemosaicNet::kSpatialLoss / 2;
  const int input_size  = cfg.tile_size + 2 * border;
  const int output_size = cfg.tile_size;
  result.input_size     = input_size;
  result.output_size    = output_size;

  RawCfaPattern pattern = DemosaicNetTrainingPattern(
      fixture.kind == FixtureKind::Bayer ? RawCfaKind::Bayer2x2 : RawCfaKind::XTrans6x6);

  const unsigned seed =
      fixture.kind == FixtureKind::Bayer ? 0xBA5E11u : 0x7A511u;
  cv::Mat host_cfa = MakeSyntheticCfa(input_size, seed);

  cv::cuda::GpuMat gpu_cfa(host_cfa);
  cv::cuda::GpuMat rgb;
  CUDA::NeuralDemosaicWorkspace workspace;
  CUDA::NeuralDemosaicOptions   options;
  options.workspace = &workspace;

  auto& cache = DemosaicNetModelCache::Instance();
  cache.Unload(fixture.variant);
  const auto cold_start = Clock::now();
  if (!cache.EnsureLoaded(fixture.variant)) {
    throw std::runtime_error("tile cold model load failed: " + cache.LastError());
  }
  result.cold_load_ms = ElapsedMs(cold_start);
  options.model_cache = &cache;
  {
    const auto check =
        CUDA::DemosaicWithNeuralEngine(gpu_cfa, pattern, rgb, nullptr, options);
    if (!check.succeeded) {
      throw std::runtime_error("tile correctness failed: " + check.error);
    }
    if (rgb.rows != output_size || rgb.cols != output_size || rgb.type() != CV_32FC3) {
      throw std::runtime_error("tile correctness shape mismatch");
    }
    cv::Mat host_rgb;
    rgb.download(host_rgb);
    if (!std::isfinite(host_rgb.at<cv::Vec3f>(0, 0)[0])) {
      throw std::runtime_error("tile correctness non-finite");
    }
  }

  for (int i = 0; i < cfg.warmup; ++i) {
    const auto warm = CUDA::DemosaicWithNeuralEngine(gpu_cfa, pattern, rgb, nullptr, options);
    if (!warm.succeeded) {
      throw std::runtime_error("tile warm-up failed: " + warm.error);
    }
  }
  cuda::nn::CheckCuda(cudaDeviceSynchronize(), "tile warm-up sync");

  result.allocation_gen_start = workspace.allocation_generation();
  std::vector<double> wall_samples;
  std::vector<double> cuda_samples;
  wall_samples.reserve(static_cast<std::size_t>(cfg.iterations));
  cuda_samples.reserve(static_cast<std::size_t>(cfg.iterations));

  perf::CudaEventRange range;
  for (int i = 0; i < cfg.iterations; ++i) {
    const auto t0 = Clock::now();
    range.RecordStart(nullptr);
    const auto timed =
        CUDA::DemosaicWithNeuralEngine(gpu_cfa, pattern, rgb, nullptr, options);
    range.RecordStop(nullptr);
    if (!timed.succeeded) {
      throw std::runtime_error("tile timed forward failed: " + timed.error);
    }
    const float cuda_ms = range.ElapsedMs();
    const double wall_ms = ElapsedMs(t0);
    wall_samples.push_back(wall_ms);
    cuda_samples.push_back(static_cast<double>(cuda_ms));
  }
  result.allocation_gen_end = workspace.allocation_generation();
  result.allocation_stable  = result.allocation_gen_start == result.allocation_gen_end;
  if (!result.allocation_stable) {
    throw std::runtime_error("NeuralDemosaicWorkspace allocation_generation changed during timed "
                             "tile iterations (grew after warm-up)");
  }

  result.wall_stats = perf::ComputeTimingStats(std::move(wall_samples));
  result.cuda_stats = perf::ComputeTimingStats(std::move(cuda_samples));

  if (cfg.profile_ranges) {
    // One instrumented pass: pack+forward is inside DemosaicWithNeuralEngine; report
    // whole-engine CUDA event as "forward" when fine-grained hooks are unavailable.
    perf::CudaEventRange fwd;
    fwd.RecordStart(nullptr);
    const auto profiled =
        CUDA::DemosaicWithNeuralEngine(gpu_cfa, pattern, rgb, nullptr, options);
    fwd.RecordStop(nullptr);
    if (!profiled.succeeded) {
      throw std::runtime_error("profile-ranges forward failed: " + profiled.error);
    }
    const float ms = fwd.ElapsedMs();
    std::ostringstream oss;
    oss << "\"forward_ms\":" << perf::JsonNumber(ms);
    result.profile_json = oss.str();
  }

  return result;
}

struct ConvLayer {
  std::string name;
  int         cin       = 0;
  int         cout      = 0;
  int         k         = 1;
  int         s         = 1;
  int         in_h      = 0;
  int         in_w      = 0;
  bool        bias_relu = true;
};

[[nodiscard]] auto BuildBayerConvLayers(const int tile_out) -> std::vector<ConvLayer> {
  const int hin = tile_out + BayerDemosaicNet::kSpatialLoss;
  const int win = hin;
  const int ph  = hin / 2;
  const int pw  = win / 2;
  std::vector<ConvLayer> layers;
  layers.push_back({"pack_mosaick", 3, 4, 2, 2, hin, win, false});
  int h = ph;
  int w = pw;
  layers.push_back({"conv1", 4, 64, 3, 1, h, w, true});
  h -= 2;
  w -= 2;
  for (int i = 2; i <= 14; ++i) {
    layers.push_back({"conv" + std::to_string(i), 64, 64, 3, 1, h, w, true});
    h -= 2;
    w -= 2;
  }
  layers.push_back({"conv15", 64, 128, 3, 1, h, w, true});
  h -= 2;
  w -= 2;
  // residual uses 64-channel filtered tensor (slice of 128); spatial is post-conv15
  layers.push_back({"residual", 64, 12, 1, 1, h, w, false});
  const int uh = h * 2;
  const int uw = w * 2;
  layers.push_back({"post_conv1", 6, 64, 3, 1, uh, uw, true});
  layers.push_back({"output", 64, 3, 1, 1, uh - 2, uw - 2, false});
  return layers;
}

[[nodiscard]] auto BuildXTransConvLayers(const int tile_out) -> std::vector<ConvLayer> {
  const int hin = tile_out + XTransDemosaicNet::kSpatialLoss;
  const int win = hin;
  std::vector<ConvLayer> layers;
  int h = hin;
  int w = win;
  for (int i = 1; i <= 11; ++i) {
    const int cin = (i == 1) ? 3 : 64;
    layers.push_back({"conv" + std::to_string(i), cin, 64, 3, 1, h, w, true});
    h -= 2;
    w -= 2;
  }
  layers.push_back({"post_conv1", 67, 64, 3, 1, h, w, true});
  layers.push_back({"output", 64, 3, 1, 1, h - 2, w - 2, false});
  return layers;
}

struct ConvLayerResult {
  std::string       name;
  int               in_h = 0;
  int               in_w = 0;
  int               cin  = 0;
  int               cout = 0;
  int               k    = 0;
  int               s    = 0;
  perf::TimingStats cuda_stats;
};

struct ConvFixtureResult {
  std::string                fixture_name;
  std::vector<ConvLayerResult> layers;
};

auto RunConvMode(const FixtureSpec& fixture, const HarnessConfig& cfg) -> ConvFixtureResult {
  ConvFixtureResult result;
  result.fixture_name = fixture.name;
  const auto layers =
      fixture.kind == FixtureKind::Bayer ? BuildBayerConvLayers(cfg.tile_size)
                                         : BuildXTransConvLayers(cfg.tile_size);

  std::mt19937 rng(42);
  std::uniform_real_distribution<float> dist(-0.05F, 0.05F);

  for (const auto& layer : layers) {
    const int out_h = cuda::nn::Conv2dOutputSize(layer.in_h, 0, 1, layer.k, layer.s);
    const int out_w = cuda::nn::Conv2dOutputSize(layer.in_w, 0, 1, layer.k, layer.s);
    if (out_h < 1 || out_w < 1) {
      throw std::runtime_error("conv layer " + layer.name + " has non-positive output size");
    }

    const std::size_t in_numel  = static_cast<std::size_t>(layer.cin) * layer.in_h * layer.in_w;
    const std::size_t out_numel = static_cast<std::size_t>(layer.cout) * out_h * out_w;
    const std::size_t w_numel =
        static_cast<std::size_t>(layer.cout) * layer.cin * layer.k * layer.k;

    cuda::nn::DeviceBufferF32 in_buf(in_numel);
    cuda::nn::DeviceBufferF32 out_buf(out_numel);
    cuda::nn::DeviceBufferF32 w_buf(w_numel);
    cuda::nn::DeviceBufferF32 b_buf(static_cast<std::size_t>(layer.cout));

    std::vector<float> host_w(w_numel);
    std::vector<float> host_b(static_cast<std::size_t>(layer.cout));
    std::vector<float> host_in(in_numel);
    for (auto& v : host_w) {
      v = dist(rng);
    }
    for (auto& v : host_b) {
      v = dist(rng);
    }
    for (auto& v : host_in) {
      v = dist(rng) + 0.5F;
    }
    w_buf.Upload(host_w);
    b_buf.Upload(host_b);
    in_buf.Upload(host_in);

    auto in_tensor = cuda::nn::DeviceTensor::Contiguous(
        in_buf.data(), {1, layer.cin, layer.in_h, layer.in_w});
    auto out_tensor =
        cuda::nn::DeviceTensor::Contiguous(out_buf.data(), {1, layer.cout, out_h, out_w});

    cuda::nn::Conv2dParams params;
    params.in_channels  = layer.cin;
    params.out_channels = layer.cout;
    params.kH = params.kW = layer.k;
    params.sH = params.sW = layer.s;
    params.weight         = w_buf.data();
    params.bias           = b_buf.data();

    auto launch = [&] {
      if (layer.bias_relu) {
        cuda::nn::Conv2dBiasRelu(in_tensor, out_tensor, params, nullptr, nullptr);
      } else {
        cuda::nn::Conv2d(in_tensor, out_tensor, params, nullptr, nullptr);
      }
    };

    for (int i = 0; i < cfg.warmup; ++i) {
      launch();
    }
    cuda::nn::CheckCuda(cudaDeviceSynchronize(), "conv warm-up");

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(cfg.iterations));
    perf::CudaEventRange range;
    for (int i = 0; i < cfg.iterations; ++i) {
      range.RecordStart(nullptr);
      launch();
      range.RecordStop(nullptr);
      samples.push_back(static_cast<double>(range.ElapsedMs()));
    }

    ConvLayerResult lr;
    lr.name       = layer.name;
    lr.in_h       = layer.in_h;
    lr.in_w       = layer.in_w;
    lr.cin        = layer.cin;
    lr.cout       = layer.cout;
    lr.k          = layer.k;
    lr.s          = layer.s;
    lr.cuda_stats = perf::ComputeTimingStats(std::move(samples));
    result.layers.push_back(std::move(lr));
  }
  return result;
}

void PrintDeviceHeader(const perf::DeviceInfo& dev) {
  std::cout << "GPU: " << dev.name << "  CC " << dev.compute_major << "." << dev.compute_minor
            << "  SMs=" << dev.multi_processor_count << "  VRAM="
            << (static_cast<double>(dev.total_global_mem_bytes) / (1024.0 * 1024.0 * 1024.0))
            << " GiB\n";
  std::cout << "Driver " << perf::FormatCudaVersion(dev.driver_version) << "  Runtime "
            << perf::FormatCudaVersion(dev.runtime_version) << "  commit " << ALCEDO_GIT_COMMIT
            << "\n";
}

void AppendFullJson(std::string& out, const FullFixtureResult& r, const bool trailing) {
  out += "{";
  perf::AppendJsonKeyString(out, "fixture", r.fixture_name);
  perf::AppendJsonKeyString(out, "path", r.fixture_path);
  perf::AppendJsonKeyInt(out, "raw_width", r.raw_width);
  perf::AppendJsonKeyInt(out, "raw_height", r.raw_height);
  perf::AppendJsonKeyInt(out, "active_width", r.active_width);
  perf::AppendJsonKeyInt(out, "active_height", r.active_height);
  perf::AppendJsonKeyNumber(out, "active_megapixels", r.active_megapixels);
  perf::AppendJsonKeyInt(out, "tile_count_est", r.tile_count);
  perf::AppendJsonKeyInt(out, "tile_size", r.tile_size);
  perf::AppendJsonKeyInt(out, "lanes", r.lanes);
  perf::AppendJsonKeyNumber(out, "cold_load_ms", r.cold_load_ms);
  if (r.legacy_stats.count > 0) {
    perf::AppendJsonTimingStats(out, "legacy_full_process_hot_ms", r.legacy_stats);
  }
  if (r.neural_stats.count > 0) {
    perf::AppendJsonTimingStats(out, "neural_full_process_hot_ms", r.neural_stats);
  }
  if (r.legacy_stats.count > 0 && r.neural_stats.count > 0 && r.legacy_stats.median_ms > 0.0) {
    perf::AppendJsonKeyNumber(out, "neural_legacy_p50_ratio",
                              r.neural_stats.median_ms / r.legacy_stats.median_ms);
    perf::AppendJsonKeyNumber(out, "neural_legacy_p95_ratio",
                              r.neural_stats.p95_ms / std::max(r.legacy_stats.p95_ms, 1e-9));
  }
  perf::AppendJsonKeyNumber(
      out, "legacy_mp_s",
      perf::ActiveMegapixelsPerSecond(r.legacy_stats.median_ms, r.active_megapixels));
  perf::AppendJsonKeyNumber(
      out, "neural_mp_s",
      perf::ActiveMegapixelsPerSecond(r.neural_stats.median_ms, r.active_megapixels));
  perf::AppendJsonKeyInt(out, "cuda_free_bytes_after",
                         static_cast<std::int64_t>(r.free_bytes_after));
  perf::AppendJsonKeyInt(out, "cuda_total_bytes", static_cast<std::int64_t>(r.total_bytes_after),
                         false);
  out += trailing ? "}," : "}";
}

}  // namespace
}  // namespace alcedo

int main(int argc, char** argv) {
  using namespace alcedo;
  try {
    const HarnessConfig cfg = ParseArgs(argc, argv);
    EnsureCuda();
    const perf::DeviceInfo device = perf::QueryDeviceInfo(0);
    PrintDeviceHeader(device);

    std::vector<FixtureSpec> fixtures;
    if (cfg.run_bayer) {
      FixtureSpec s;
      s.kind    = FixtureKind::Bayer;
      s.name    = "bayer_d800e";
      s.path    = cfg.fixture_override.empty() ? DefaultBayerPath() : cfg.fixture_override;
      s.variant = DemosaicNetVariant::Bayer;
      fixtures.push_back(std::move(s));
    }
    if (cfg.run_xtrans) {
      FixtureSpec s;
      s.kind    = FixtureKind::XTrans;
      s.name    = "xtrans_xt5";
      s.path    = (cfg.fixture_override.empty() || cfg.run_bayer) ? DefaultXTransPath()
                                                                  : cfg.fixture_override;
      s.variant = DemosaicNetVariant::XTrans;
      fixtures.push_back(std::move(s));
    }

    std::string json = "{";
    perf::AppendJsonKeyString(json, "git_commit", ALCEDO_GIT_COMMIT);
    perf::AppendJsonKeyString(json, "gpu_name", device.name);
    perf::AppendJsonKeyString(json, "compute_capability",
                              std::to_string(device.compute_major) + "." +
                                  std::to_string(device.compute_minor));
    perf::AppendJsonKeyString(json, "driver_version",
                              perf::FormatCudaVersion(device.driver_version));
    perf::AppendJsonKeyString(json, "runtime_version",
                              perf::FormatCudaVersion(device.runtime_version));
    perf::AppendJsonKeyInt(json, "lanes", cfg.lanes);
    perf::AppendJsonKeyInt(json, "tile_size", cfg.tile_size);
    perf::AppendJsonKeyInt(json, "warmup", cfg.warmup);
    perf::AppendJsonKeyInt(json, "iterations", cfg.iterations);
    perf::AppendJsonKeyBool(json, "profile_ranges", cfg.profile_ranges);

    const char* mode_str = cfg.mode == ModeKind::Full   ? "full"
                           : cfg.mode == ModeKind::Tile ? "tile"
                                                        : "conv";
    perf::AppendJsonKeyString(json, "mode", mode_str);

    if (cfg.mode == ModeKind::Full) {
      json += "\"fixtures\":[";
      for (std::size_t i = 0; i < fixtures.size(); ++i) {
        const auto r = RunFullMode(fixtures[i], cfg);
        std::cout << "\n--- full / " << r.fixture_name << " ---\n";
        std::cout << "  raw " << r.raw_width << "x" << r.raw_height << "  active "
                  << r.active_width << "x" << r.active_height << " ("
                  << std::setprecision(3) << r.active_megapixels << " MP)\n";
        std::cout << "  estimated tiles=" << r.tile_count << "  cold_load_ms=" << r.cold_load_ms
                  << "\n";
        if (r.legacy_stats.count > 0) {
          perf::PrintTimingTable("Legacy full_process_hot_ms", r.legacy_stats,
                                 r.active_megapixels, r.tile_count);
        }
        if (r.neural_stats.count > 0) {
          perf::PrintTimingTable("Neural full_process_hot_ms", r.neural_stats,
                                 r.active_megapixels, r.tile_count);
        }
        if (r.legacy_stats.count > 0 && r.neural_stats.count > 0 &&
            r.legacy_stats.median_ms > 0.0) {
          std::cout << std::fixed << std::setprecision(3)
                    << "  neural/legacy p50 ratio="
                    << (r.neural_stats.median_ms / r.legacy_stats.median_ms)
                    << "  p95 ratio=" << (r.neural_stats.p95_ms / r.legacy_stats.p95_ms) << "\n";
          std::cout << "  stretch 100ms progress neural_p50=" << r.neural_stats.median_ms
                    << " ms\n";
        }
        AppendFullJson(json, r, i + 1 < fixtures.size());
      }
      json += "]";
    } else if (cfg.mode == ModeKind::Tile) {
      json += "\"fixtures\":[";
      for (std::size_t i = 0; i < fixtures.size(); ++i) {
        const auto r = RunTileMode(fixtures[i], cfg);
        std::cout << "\n--- tile / " << r.fixture_name << "  input " << r.input_size << "^2 -> "
                  << r.output_size << "^2 ---\n";
        std::cout << "  cold_load_ms=" << r.cold_load_ms
                  << "  allocation_generation=" << r.allocation_gen_end
                  << "  stable=" << (r.allocation_stable ? "yes" : "no") << "\n";
        perf::PrintTimingTable("Tile wall_ms", r.wall_stats, 0.0, 1);
        perf::PrintTimingTable("Tile cuda_ms", r.cuda_stats, 0.0, 1);
        json += "{";
        perf::AppendJsonKeyString(json, "fixture", r.fixture_name);
        perf::AppendJsonKeyInt(json, "input_size", r.input_size);
        perf::AppendJsonKeyInt(json, "output_size", r.output_size);
        perf::AppendJsonKeyNumber(json, "cold_load_ms", r.cold_load_ms);
        perf::AppendJsonKeyInt(json, "allocation_generation",
                               static_cast<std::int64_t>(r.allocation_gen_end));
        perf::AppendJsonKeyBool(json, "allocation_stable", r.allocation_stable);
        perf::AppendJsonTimingStats(json, "wall_ms", r.wall_stats);
        perf::AppendJsonTimingStats(json, "cuda_ms", r.cuda_stats, r.profile_json.empty());
        if (!r.profile_json.empty()) {
          json += "\"profile\":{";
          json += r.profile_json;
          json += "}";
        }
        json += (i + 1 < fixtures.size()) ? "}," : "}";
      }
      json += "]";
    } else {
      json += "\"fixtures\":[";
      for (std::size_t fi = 0; fi < fixtures.size(); ++fi) {
        const auto r = RunConvMode(fixtures[fi], cfg);
        std::cout << "\n--- conv / " << r.fixture_name << " ---\n";
        json += "{";
        perf::AppendJsonKeyString(json, "fixture", r.fixture_name);
        json += "\"layers\":[";
        for (std::size_t li = 0; li < r.layers.size(); ++li) {
          const auto& layer = r.layers[li];
          std::cout << "  " << std::setw(14) << std::left << layer.name << std::right
                    << "  " << layer.cin << "->" << layer.cout << " k=" << layer.k
                    << " s=" << layer.s << " in=" << layer.in_h << "x" << layer.in_w
                    << "  median=" << std::fixed << std::setprecision(3)
                    << layer.cuda_stats.median_ms << " ms\n";
          json += "{";
          perf::AppendJsonKeyString(json, "name", layer.name);
          perf::AppendJsonKeyInt(json, "cin", layer.cin);
          perf::AppendJsonKeyInt(json, "cout", layer.cout);
          perf::AppendJsonKeyInt(json, "k", layer.k);
          perf::AppendJsonKeyInt(json, "s", layer.s);
          perf::AppendJsonKeyInt(json, "in_h", layer.in_h);
          perf::AppendJsonKeyInt(json, "in_w", layer.in_w);
          perf::AppendJsonTimingStats(json, "cuda_ms", layer.cuda_stats, false);
          json += (li + 1 < r.layers.size()) ? "}," : "}";
        }
        json += "]";
        json += (fi + 1 < fixtures.size()) ? "}," : "}";
      }
      json += "]";
    }

    json += "}";

    if (!cfg.output_json.empty()) {
      if (cfg.output_json.has_parent_path()) {
        fs::create_directories(cfg.output_json.parent_path());
      }
      std::ofstream out(cfg.output_json, std::ios::binary);
      if (!out) {
        throw std::runtime_error("failed to write " + cfg.output_json.string());
      }
      out << json << "\n";
      std::cout << "\nWrote " << cfg.output_json.string() << "\n";
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "DemosaicNetPerfHarness error: " << e.what() << "\n";
    return 1;
  }
}
