//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
// OpenCL Neural full-frame telemetry harness (development report).
// Cold path includes first program compile / model load; hot mean excludes that.
//
//   OpenClDemosaicNetFullFrameTiming.exe
//     [--warmup 1] [--iterations 3]
//     [--wall] [--event-profile] [--boundary-profile] [--compare]
//     [--cuda-control] [--json path]
//
// --compare (default when no mode flag): one unprofiled wall series, then one
// event-profiled series; reports residual wall − device_exec and API counters.

#include <libraw/libraw.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "decoders/processor/nn/opencl_demosaicnet_cache.hpp"
#include "decoders/processor/operators/gpu/opencl_demosaicnet_programs.hpp"
#include "decoders/processor/raw_processor.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"
#include "opencl/nn/demosaicnet_stage_profiler.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_runtime.hpp"

#if defined(ALCEDO_FULLFRAME_CUDA_CONTROL) && defined(HAVE_CUDA)
#define ALCEDO_FULLFRAME_HAS_CUDA 1
#include "decoders/processor/nn/demosaicnet_profiler.hpp"
#else
#define ALCEDO_FULLFRAME_HAS_CUDA 0
#endif

namespace alcedo {
namespace {

using Clock      = std::chrono::steady_clock;
namespace fs     = std::filesystem;
namespace nn_ocl = opencl::nn;

[[nodiscard]] auto ElapsedMs(const Clock::time_point start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

[[nodiscard]] auto Mean(const std::vector<double>& samples) -> double {
  if (samples.empty()) {
    return 0.0;
  }
  return std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());
}

[[nodiscard]] auto BayerPath() -> fs::path {
  return fs::path(TEST_IMG_PATH) / "raw" / "camera" / "nikon" / "d800e" /
         "Nikon-D800e-raw-00002.nef";
}

[[nodiscard]] auto XTransPath() -> fs::path {
  return fs::path(TEST_IMG_PATH) / "raw" / "camera" / "fuji" / "xt5" / "DSCF2074.RAF";
}

enum class ProfileKind { WallOnly, EventTimestamps, BoundaryDrain };

struct RunStats {
  std::string                    name;
  std::string                    backend;
  int                            width   = 0;
  int                            height  = 0;
  int                            out_w   = 0;
  int                            out_h   = 0;
  double                         cold_ms = 0.0;
  std::vector<double>            hot_ms;
  bool                           used_neural  = false;
  ProfileKind                    profile_kind = ProfileKind::WallOnly;

  OpenClApiCounters              cold_counters{};
  OpenClApiCounters              hot_counters_mean{};  // arithmetic mean of per-hot deltas
  std::vector<OpenClApiCounters> hot_counter_samples;

  std::vector<nn_ocl::DemosaicNetStageTiming> event_stage_means;
  nn_ocl::DemosaicNetProfileSummary           event_summary{};
  bool                                        has_event_summary = false;

  // Same-session CUDA Neural control (optional).
  std::vector<double>                         cuda_hot_ms;
  double                                      cuda_hot_mean_ms = 0.0;
  bool                                        has_cuda         = false;
};

struct GpuTelemetry {
  int         temperature_c = -1;
  double      power_w       = -1.0;
  int         sm_clock_mhz  = -1;
  int         mem_clock_mhz = -1;
  std::string pstate;
  bool        available = false;
};

[[nodiscard]] auto QueryGpuTelemetry() -> GpuTelemetry {
  GpuTelemetry snap;
#if ALCEDO_FULLFRAME_HAS_CUDA
  const auto t       = QueryDemosaicNetGpuTelemetry();
  snap.temperature_c = t.temperature_c;
  snap.power_w       = t.power_w;
  snap.sm_clock_mhz  = t.sm_clock_mhz;
  snap.mem_clock_mhz = t.mem_clock_mhz;
  snap.pstate        = t.pstate;
  snap.available     = t.available;
  return snap;
#else
#if defined(_WIN32)
  FILE* pipe = _popen(
      "nvidia-smi --query-gpu=temperature.gpu,power.draw,clocks.sm,clocks.mem,pstate "
      "--format=csv,noheader,nounits",
      "r");
#else
  FILE* pipe = popen(
      "nvidia-smi --query-gpu=temperature.gpu,power.draw,clocks.sm,clocks.mem,pstate "
      "--format=csv,noheader,nounits",
      "r");
#endif
  if (pipe == nullptr) {
    return snap;
  }
  char line[512] = {};
  if (std::fgets(line, sizeof(line), pipe) != nullptr) {
    auto next = [](char*& cur) -> char* {
      if (cur == nullptr || *cur == '\0') {
        return nullptr;
      }
      char* start = cur;
      while (*cur != '\0' && *cur != ',' && *cur != '\r' && *cur != '\n') {
        ++cur;
      }
      if (*cur != '\0') {
        *cur++ = '\0';
      }
      while (*start == ' ' || *start == '\t') {
        ++start;
      }
      return start;
    };
    char* cur = line;
    if (char* t = next(cur); t != nullptr) {
      snap.temperature_c = std::atoi(t);
    }
    if (char* t = next(cur); t != nullptr) {
      snap.power_w = std::atof(t);
    }
    if (char* t = next(cur); t != nullptr) {
      snap.sm_clock_mhz = std::atoi(t);
    }
    if (char* t = next(cur); t != nullptr) {
      snap.mem_clock_mhz = std::atoi(t);
    }
    if (char* t = next(cur); t != nullptr) {
      snap.pstate    = t;
      snap.available = true;
    }
  }
#if defined(_WIN32)
  _pclose(pipe);
#else
  pclose(pipe);
#endif
  return snap;
#endif
}

[[nodiscard]] auto ProcessOnce(LibRaw& raw, const RawDemosaicMethod method,
                               const RawGpuBackend backend) -> ImageBuffer {
  RawParams params;
  params.gpu_backend_            = backend;
  params.demosaic_method_        = method;
  params.highlights_reconstruct_ = false;
  params.decode_res_             = DecodeRes::FULL;
  RawRuntimeColorContext context;
  const ushort           no_crop[4] = {};
  RawProcessor           processor(params, raw.imgdata.rawdata, raw, context, no_crop);
  return processor.Process();
}

[[nodiscard]] auto MeanCounters(const std::vector<OpenClApiCounters>& samples)
    -> OpenClApiCounters {
  OpenClApiCounters mean{};
  if (samples.empty()) {
    return mean;
  }
  for (const auto& s : samples) {
    mean.create_buffer += s.create_buffer;
    mean.create_sub_buffer += s.create_sub_buffer;
    mean.create_kernel += s.create_kernel;
    mean.release_mem_object += s.release_mem_object;
    mean.release_kernel += s.release_kernel;
    mean.h2d_bytes += s.h2d_bytes;
    mean.d2h_bytes += s.d2h_bytes;
    mean.program_builds += s.program_builds;
    mean.final_waits += s.final_waits;
    mean.queue_finish += s.queue_finish;
    mean.enqueue_ndrange += s.enqueue_ndrange;
  }
  const auto n = static_cast<std::uint64_t>(samples.size());
  mean.create_buffer /= n;
  mean.create_sub_buffer /= n;
  mean.create_kernel /= n;
  mean.release_mem_object /= n;
  mean.release_kernel /= n;
  mean.h2d_bytes /= n;
  mean.d2h_bytes /= n;
  mean.program_builds /= n;
  mean.final_waits /= n;
  mean.queue_finish /= n;
  mean.enqueue_ndrange /= n;
  return mean;
}

void AggregateStageMeans(const std::vector<std::vector<nn_ocl::DemosaicNetStageTiming>>& samples,
                         std::vector<nn_ocl::DemosaicNetStageTiming>&                    out_means,
                         nn_ocl::DemosaicNetProfileSummary& summary_mean) {
  std::map<std::string, nn_ocl::DemosaicNetStageTiming> acc;
  double                                                wall_sum   = 0.0;
  double                                                host_sum   = 0.0;
  double                                                device_sum = 0.0;
  double                                                queue_sum  = 0.0;
  double                                                submit_sum = 0.0;
  std::size_t                                           event_sum  = 0;
  std::size_t                                           n          = samples.size();
  for (const auto& sample : samples) {
    for (const auto& stage : sample) {
      auto& a = acc[stage.name];
      a.name  = stage.name;
      a.calls += stage.calls;
      a.host_enqueue_wall_ms += stage.host_enqueue_wall_ms;
      a.device_exec_ms += stage.device_exec_ms;
      a.queue_delay_ms += stage.queue_delay_ms;
      a.submit_delay_ms += stage.submit_delay_ms;
      a.total_ms += stage.total_ms;
      a.event_count += stage.event_count;
    }
  }
  // summary fields filled by caller from per-run summaries if needed
  (void)wall_sum;
  (void)host_sum;
  (void)device_sum;
  (void)queue_sum;
  (void)submit_sum;
  (void)event_sum;
  (void)summary_mean;
  out_means.clear();
  if (n == 0) {
    return;
  }
  for (auto& [name, a] : acc) {
    (void)name;
    a.calls = a.calls / n;
    a.host_enqueue_wall_ms /= static_cast<double>(n);
    a.device_exec_ms /= static_cast<double>(n);
    a.queue_delay_ms /= static_cast<double>(n);
    a.submit_delay_ms /= static_cast<double>(n);
    a.total_ms /= static_cast<double>(n);
    a.event_count = a.event_count / n;
    out_means.push_back(a);
  }
}

auto TimeFixtureOpenCl(const char* name, const fs::path& path, const RawCfaKind expected_kind,
                       const OpenClDemosaicNetVariant variant, const int warmup,
                       const int iterations, const ProfileKind profile_kind) -> RunStats {
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
  stats.name         = name;
  stats.width        = raw->imgdata.sizes.raw_width;
  stats.height       = raw->imgdata.sizes.raw_height;
  stats.profile_kind = profile_kind;
  stats.backend      = "OpenCL";

  auto& cache        = OpenClDemosaicNetModelCache::Instance();
  cache.Unload(variant);

  OpenClApiCounterScope counter_scope(true);
  ResetOpenClApiCounters();

  // Cold: first Neural process (may compile programs + load weights).
  {
    const auto before   = SnapshotOpenClApiCounters();
    const auto t0       = Clock::now();
    auto       out      = ProcessOnce(*raw, RawDemosaicMethod::NeuralEngine, RawGpuBackend::OpenCL);
    stats.cold_ms       = ElapsedMs(t0);
    const auto after    = SnapshotOpenClApiCounters();
    stats.cold_counters = DeltaOpenClApiCounters(before, after);
    if (!out.gpu_data_valid_ || out.GetGPUBackend() != GpuBackendKind::OpenCL) {
      throw std::runtime_error(std::string(name) + ": cold output not on OpenCL");
    }
    stats.out_w       = out.GetOpenClImage().Width();
    stats.out_h       = out.GetOpenClImage().Height();
    stats.used_neural = cache.IsLoaded(variant);
  }

  for (int i = 0; i < warmup; ++i) {
    (void)ProcessOnce(*raw, RawDemosaicMethod::NeuralEngine, RawGpuBackend::OpenCL);
  }

  stats.hot_ms.reserve(static_cast<std::size_t>(iterations));
  stats.hot_counter_samples.reserve(static_cast<std::size_t>(iterations));
  std::vector<std::vector<nn_ocl::DemosaicNetStageTiming>> stage_samples;
  std::vector<nn_ocl::DemosaicNetProfileSummary>           summary_samples;

  for (int i = 0; i < iterations; ++i) {
    const auto before = SnapshotOpenClApiCounters();

    if (profile_kind == ProfileKind::WallOnly) {
      const auto t0  = Clock::now();
      auto       out = ProcessOnce(*raw, RawDemosaicMethod::NeuralEngine, RawGpuBackend::OpenCL);
      stats.hot_ms.push_back(ElapsedMs(t0));
      if (!out.gpu_data_valid_ || out.GetGPUBackend() != GpuBackendKind::OpenCL) {
        throw std::runtime_error(std::string(name) + ": hot output not on OpenCL");
      }
    } else {
      const auto                       mode = profile_kind == ProfileKind::EventTimestamps
                                                  ? nn_ocl::DemosaicNetProfileMode::EventTimestamps
                                                  : nn_ocl::DemosaicNetProfileMode::BoundaryDrain;
      nn_ocl::DemosaicNetStageProfiler profiler(mode);
      {
        nn_ocl::DemosaicNetStageProfilerScope scope(&profiler);
        profiler.BeginWall();
        auto out = ProcessOnce(*raw, RawDemosaicMethod::NeuralEngine, RawGpuBackend::OpenCL);
        profiler.EndWall();
        if (profile_kind == ProfileKind::EventTimestamps) {
          profiler.CollectEventTimestamps();
        }
        if (!out.gpu_data_valid_ || out.GetGPUBackend() != GpuBackendKind::OpenCL) {
          throw std::runtime_error(std::string(name) + ": hot output not on OpenCL");
        }
      }
      const auto summary = profiler.Summary();
      stats.hot_ms.push_back(summary.wall_ms);
      stage_samples.push_back(profiler.Timings());
      summary_samples.push_back(summary);
    }

    const auto after = SnapshotOpenClApiCounters();
    stats.hot_counter_samples.push_back(DeltaOpenClApiCounters(before, after));
  }

  stats.hot_counters_mean = MeanCounters(stats.hot_counter_samples);

  if (!summary_samples.empty()) {
    stats.has_event_summary = true;
    AggregateStageMeans(stage_samples, stats.event_stage_means, stats.event_summary);
    // Mean summary scalars.
    for (const auto& s : summary_samples) {
      stats.event_summary.wall_ms += s.wall_ms;
      stats.event_summary.host_enqueue_wall_ms += s.host_enqueue_wall_ms;
      stats.event_summary.device_exec_sum_ms += s.device_exec_sum_ms;
      stats.event_summary.queue_delay_sum_ms += s.queue_delay_sum_ms;
      stats.event_summary.submit_delay_sum_ms += s.submit_delay_sum_ms;
      stats.event_summary.residual_wall_minus_device_ms += s.residual_wall_minus_device_ms;
      stats.event_summary.event_count += s.event_count;
      stats.event_summary.events_collected =
          stats.event_summary.events_collected || s.events_collected;
      stats.event_summary.used_profiling_queue =
          stats.event_summary.used_profiling_queue || s.used_profiling_queue;
      stats.event_summary.mode = s.mode;
    }
    const double n = static_cast<double>(summary_samples.size());
    stats.event_summary.wall_ms /= n;
    stats.event_summary.host_enqueue_wall_ms /= n;
    stats.event_summary.device_exec_sum_ms /= n;
    stats.event_summary.queue_delay_sum_ms /= n;
    stats.event_summary.submit_delay_sum_ms /= n;
    stats.event_summary.residual_wall_minus_device_ms /= n;
    stats.event_summary.event_count =
        static_cast<std::size_t>(std::llround(stats.event_summary.event_count / n));
  }

  raw->recycle();
  return stats;
}

#if ALCEDO_FULLFRAME_HAS_CUDA
auto TimeCudaControl(const fs::path& path, const int warmup, const int iterations)
    -> std::vector<double> {
  auto raw = std::make_unique<LibRaw>();
  if (raw->open_file(path.string().c_str()) != LIBRAW_SUCCESS) {
    throw std::runtime_error("LibRaw open failed for CUDA control: " + path.string());
  }
  if (raw->unpack() != LIBRAW_SUCCESS) {
    throw std::runtime_error("LibRaw unpack failed for CUDA control: " + path.string());
  }
  // Cold + warmup
  (void)ProcessOnce(*raw, RawDemosaicMethod::NeuralEngine, RawGpuBackend::CUDA);
  for (int i = 0; i < warmup; ++i) {
    (void)ProcessOnce(*raw, RawDemosaicMethod::NeuralEngine, RawGpuBackend::CUDA);
  }
  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(iterations));
  for (int i = 0; i < iterations; ++i) {
    const auto t0  = Clock::now();
    auto       out = ProcessOnce(*raw, RawDemosaicMethod::NeuralEngine, RawGpuBackend::CUDA);
    samples.push_back(ElapsedMs(t0));
    if (!out.gpu_data_valid_) {
      throw std::runtime_error("CUDA control: invalid output");
    }
  }
  raw->recycle();
  return samples;
}
#endif

void PrintCounters(const char* label, const OpenClApiCounters& c) {
  std::cout << "  " << label << ": " << FormatOpenClApiCounters(c) << "\n";
}

void PrintStats(const RunStats& s) {
  std::cout << "\n=== " << s.name << " ===\n";
  std::cout << "  CFA input:     " << s.width << " x " << s.height << "\n";
  std::cout << "  Output RGBA:   " << s.out_w << " x " << s.out_h << "\n";
  std::cout << "  Backend:       " << s.backend << "\n";
  std::cout << "  Neural cache:  " << (s.used_neural ? "loaded" : "not loaded (Legacy?)") << "\n";
  std::cout << "  Profile kind:  ";
  switch (s.profile_kind) {
    case ProfileKind::WallOnly:
      std::cout << "wall (unprofiled product path)\n";
      break;
    case ProfileKind::EventTimestamps:
      std::cout << "event timestamps (profiling queue, no mid-network clFinish)\n";
      break;
    case ProfileKind::BoundaryDrain:
      std::cout << "boundary drain (diagnostic clFinish; not product timing)\n";
      break;
  }
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "  Cold (incl. compile/load): " << s.cold_ms << " ms\n";
  std::cout << "  Hot runs:  ";
  for (std::size_t i = 0; i < s.hot_ms.size(); ++i) {
    if (i != 0) {
      std::cout << ", ";
    }
    std::cout << s.hot_ms[i] << " ms";
  }
  std::cout << "\n";
  std::cout << "  Hot mean of " << s.hot_ms.size() << ":         " << Mean(s.hot_ms) << " ms\n";
  PrintCounters("Cold counters", s.cold_counters);
  PrintCounters("Hot mean counters", s.hot_counters_mean);
  std::cout << "  Hot H2D bytes mean: " << s.hot_counters_mean.h2d_bytes
            << (s.hot_counters_mean.h2d_bytes == 0 ? " (zero — proven)" : " (NON-ZERO)") << "\n";
  std::cout << "  Hot D2H bytes mean: " << s.hot_counters_mean.d2h_bytes
            << (s.hot_counters_mean.d2h_bytes == 0 ? " (zero — proven)" : " (NON-ZERO)") << "\n";

  if (s.has_event_summary) {
    const auto& e = s.event_summary;
    std::cout << "  Event summary (mean across profiled hot runs):\n";
    std::cout << "    wall_ms:              " << e.wall_ms << "\n";
    std::cout << "    host_enqueue_wall_ms: " << e.host_enqueue_wall_ms << "\n";
    std::cout << "    device_exec_sum_ms:   " << e.device_exec_sum_ms << "\n";
    std::cout << "    queue_delay_sum_ms:   " << e.queue_delay_sum_ms << "\n";
    std::cout << "    submit_delay_sum_ms:  " << e.submit_delay_sum_ms << "\n";
    std::cout << "    residual (wall-device): " << e.residual_wall_minus_device_ms << " ms\n";
    std::cout << "    event_count:          " << e.event_count << "\n";
    std::cout << "    profiling_queue:      " << (e.used_profiling_queue ? "yes" : "no") << "\n";
    std::cout << "  Stage device/host breakdown:\n";
    for (const auto& stage : s.event_stage_means) {
      std::cout << "    " << stage.name << ": device=" << stage.device_exec_ms
                << " ms host_enqueue=" << stage.host_enqueue_wall_ms
                << " ms queue_delay=" << stage.queue_delay_ms << " ms calls=" << stage.calls
                << " events=" << stage.event_count << "\n";
    }
  }

  if (s.has_cuda) {
    std::cout << "  CUDA control hot samples: ";
    for (std::size_t i = 0; i < s.cuda_hot_ms.size(); ++i) {
      if (i != 0) {
        std::cout << ", ";
      }
      std::cout << s.cuda_hot_ms[i] << " ms";
    }
    std::cout << "\n  CUDA control hot mean: " << s.cuda_hot_mean_ms << " ms\n";
  }
}

[[nodiscard]] auto EscapeJson(const std::string& s) -> std::string {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

void WriteJson(const fs::path& path, const std::vector<RunStats>& runs,
               const OpenClDeviceCapabilities& caps, const GpuTelemetry& telem_before,
               const GpuTelemetry& telem_after, const std::string& neural_build_options) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(4);
  oss << "{\n";
  oss << "  \"measurement\": \"opencl_demosaicnet_full_frame\",\n";
  oss << "  \"device\": {\n";
  oss << "    \"name\": \"" << EscapeJson(caps.name) << "\",\n";
  oss << "    \"vendor\": \"" << EscapeJson(caps.vendor) << "\",\n";
  oss << "    \"driver_version\": \"" << EscapeJson(caps.driver_version) << "\",\n";
  oss << "    \"device_version\": \"" << EscapeJson(caps.device_version) << "\",\n";
  oss << "    \"opencl_c_version\": \"" << EscapeJson(caps.opencl_c_version) << "\",\n";
  oss << "    \"extensions\": \"" << EscapeJson(caps.extensions) << "\",\n";
  oss << "    \"command_buffer_extension\": "
      << (caps.SupportsExtension("cl_khr_command_buffer") ? "true" : "false") << ",\n";
  oss << "    \"max_clock_frequency_mhz\": " << caps.max_clock_frequency_mhz << ",\n";
  oss << "    \"compute_units\": " << caps.compute_units << ",\n";
  oss << "    \"global_memory_bytes\": " << caps.global_memory_bytes << "\n";
  oss << "  },\n";
  oss << "  \"neural_build_options\": \"" << EscapeJson(neural_build_options) << "\",\n";
  oss << "  \"telemetry_before\": {\"available\":" << (telem_before.available ? "true" : "false")
      << ",\"temperature_c\":" << telem_before.temperature_c
      << ",\"power_w\":" << telem_before.power_w
      << ",\"sm_clock_mhz\":" << telem_before.sm_clock_mhz
      << ",\"mem_clock_mhz\":" << telem_before.mem_clock_mhz << ",\"pstate\":\""
      << EscapeJson(telem_before.pstate) << "\"},\n";
  oss << "  \"telemetry_after\": {\"available\":" << (telem_after.available ? "true" : "false")
      << ",\"temperature_c\":" << telem_after.temperature_c
      << ",\"power_w\":" << telem_after.power_w << ",\"sm_clock_mhz\":" << telem_after.sm_clock_mhz
      << ",\"mem_clock_mhz\":" << telem_after.mem_clock_mhz << ",\"pstate\":\""
      << EscapeJson(telem_after.pstate) << "\"},\n";
  oss << "  \"runs\": [\n";
  for (std::size_t i = 0; i < runs.size(); ++i) {
    const auto& r = runs[i];
    if (i != 0) {
      oss << ",\n";
    }
    oss << "    {\n";
    oss << "      \"name\": \"" << EscapeJson(r.name) << "\",\n";
    oss << "      \"backend\": \"" << EscapeJson(r.backend) << "\",\n";
    oss << "      \"profile_kind\": \"";
    switch (r.profile_kind) {
      case ProfileKind::WallOnly:
        oss << "wall";
        break;
      case ProfileKind::EventTimestamps:
        oss << "event_timestamps";
        break;
      case ProfileKind::BoundaryDrain:
        oss << "boundary_drain";
        break;
    }
    oss << "\",\n";
    oss << "      \"input_wh\": [" << r.width << ", " << r.height << "],\n";
    oss << "      \"output_wh\": [" << r.out_w << ", " << r.out_h << "],\n";
    oss << "      \"used_neural\": " << (r.used_neural ? "true" : "false") << ",\n";
    oss << "      \"cold_ms\": " << r.cold_ms << ",\n";
    oss << "      \"hot_ms\": [";
    for (std::size_t j = 0; j < r.hot_ms.size(); ++j) {
      if (j != 0) {
        oss << ", ";
      }
      oss << r.hot_ms[j];
    }
    oss << "],\n";
    oss << "      \"hot_mean_ms\": " << Mean(r.hot_ms) << ",\n";
    oss << "      \"cold_counters\": {" << OpenClApiCountersToJsonObjectBody(r.cold_counters)
        << "},\n";
    oss << "      \"hot_counters_mean\": {"
        << OpenClApiCountersToJsonObjectBody(r.hot_counters_mean) << "},\n";
    if (r.has_event_summary) {
      oss << "      \"event_summary\": {\n";
      oss << "        \"wall_ms\": " << r.event_summary.wall_ms << ",\n";
      oss << "        \"host_enqueue_wall_ms\": " << r.event_summary.host_enqueue_wall_ms << ",\n";
      oss << "        \"device_exec_sum_ms\": " << r.event_summary.device_exec_sum_ms << ",\n";
      oss << "        \"queue_delay_sum_ms\": " << r.event_summary.queue_delay_sum_ms << ",\n";
      oss << "        \"submit_delay_sum_ms\": " << r.event_summary.submit_delay_sum_ms << ",\n";
      oss << "        \"residual_wall_minus_device_ms\": "
          << r.event_summary.residual_wall_minus_device_ms << ",\n";
      oss << "        \"event_count\": " << r.event_summary.event_count << ",\n";
      oss << "        \"used_profiling_queue\": "
          << (r.event_summary.used_profiling_queue ? "true" : "false") << "\n";
      oss << "      },\n";
      oss << "      \"stages\": [\n";
      for (std::size_t j = 0; j < r.event_stage_means.size(); ++j) {
        const auto& t = r.event_stage_means[j];
        if (j != 0) {
          oss << ",\n";
        }
        oss << "        {\"name\":\"" << EscapeJson(t.name) << "\",\"calls\":" << t.calls
            << ",\"host_enqueue_wall_ms\":" << t.host_enqueue_wall_ms
            << ",\"device_exec_ms\":" << t.device_exec_ms
            << ",\"queue_delay_ms\":" << t.queue_delay_ms
            << ",\"submit_delay_ms\":" << t.submit_delay_ms << ",\"total_ms\":" << t.total_ms
            << ",\"event_count\":" << t.event_count << "}";
      }
      oss << "\n      ],\n";
    }
    if (r.has_cuda) {
      oss << "      \"cuda_hot_ms\": [";
      for (std::size_t j = 0; j < r.cuda_hot_ms.size(); ++j) {
        if (j != 0) {
          oss << ", ";
        }
        oss << r.cuda_hot_ms[j];
      }
      oss << "],\n";
      oss << "      \"cuda_hot_mean_ms\": " << r.cuda_hot_mean_ms << ",\n";
    }
    oss << "      \"ok\": true\n";
    oss << "    }";
  }
  oss << "\n  ]\n";
  oss << "}\n";

  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write JSON: " + path.string());
  }
  out << oss.str();
}

}  // namespace
}  // namespace alcedo

auto main(int argc, char** argv) -> int {
  using namespace alcedo;

  int      warmup       = 1;
  int      iterations   = 3;
  bool     do_wall      = false;
  bool     do_event     = false;
  bool     do_boundary  = false;
  bool     do_compare   = false;
  bool     cuda_control = false;
  fs::path json_path;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if ((arg == "--warmup" || arg == "-w") && i + 1 < argc) {
      warmup = std::stoi(argv[++i]);
    } else if ((arg == "--iterations" || arg == "-n") && i + 1 < argc) {
      iterations = std::stoi(argv[++i]);
    } else if (arg == "--wall") {
      do_wall = true;
    } else if (arg == "--event-profile") {
      do_event = true;
    } else if (arg == "--boundary-profile" || arg == "--stage-profile") {
      do_boundary = true;
    } else if (arg == "--compare") {
      do_compare = true;
    } else if (arg == "--cuda-control") {
      cuda_control = true;
    } else if ((arg == "--json" || arg == "-o") && i + 1 < argc) {
      json_path = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: OpenClDemosaicNetFullFrameTiming [options]\n"
                << "  --warmup N           warm-up decodes after cold (default 1)\n"
                << "  --iterations N       measured hot runs (default 3)\n"
                << "  --wall               unprofiled wall timing only\n"
                << "  --event-profile      event-timestamp telemetry (profiling queue)\n"
                << "  --boundary-profile   diagnostic boundary clFinish profile\n"
                << "  --compare            wall then event (default if no mode flag)\n"
                << "  --cuda-control       alternate CUDA Neural same-session control\n"
                << "  --json path          write machine-readable artifact\n";
      return 0;
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return 2;
    }
  }
  if (warmup < 0 || iterations < 1) {
    std::cerr << "invalid warmup/iterations\n";
    return 2;
  }
  if (!do_wall && !do_event && !do_boundary && !do_compare) {
    do_compare = true;
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
  const auto& caps = ctx.Capabilities();
  std::cout << "OpenCL device: " << caps.name << "\n";
  std::cout << "OpenCL vendor: " << caps.vendor << "\n";
  std::cout << "Driver:        " << caps.driver_version << "\n";
  std::cout << "Device OpenCL: " << caps.device_version << " / " << caps.opencl_c_version << "\n";
  std::cout << "Max clock MHz: " << caps.max_clock_frequency_mhz << "\n";
  std::cout << "Build type:    "
#if defined(ALCEDO_OPENCL_DEMOSAICNET_RELEASE_BUILD) || defined(NDEBUG)
            << "Release\n";
#else
            << "Debug\n";
#endif
  const std::string neural_build_options =
      std::string("bayer_conv=[") + OpenCL::DemosaicNet::kBayerConvBuildOptions +
      "] xtrans_conv=[" + OpenCL::DemosaicNet::kXTransConvBuildOptions + "] structural=[" +
      OpenCL::DemosaicNet::kStructuralBuildOptions + "]";
  std::cout << "Neural options: " << neural_build_options << "\n";
  std::cout << "Warmup: " << warmup << "  measured iterations: " << iterations << "\n";
  std::cout << "OpenCL DemosaicNet telemetry: wall / event timestamps / API counters.\n";
  std::cout << "Command buffer replay: "
            << (caps.SupportsExtension("cl_khr_command_buffer") ? "available" : "unavailable")
            << " (ordinary in-order path retained)\n";

  const auto telem_before = QueryGpuTelemetry();
  if (telem_before.available) {
    std::cout << "GPU telemetry before: temp=" << telem_before.temperature_c
              << "C power=" << telem_before.power_w << "W sm=" << telem_before.sm_clock_mhz
              << "MHz mem=" << telem_before.mem_clock_mhz << "MHz pstate=" << telem_before.pstate
              << "\n";
  }

  try {
    std::vector<RunStats> all_runs;

    struct Fixture {
      const char*              name;
      fs::path                 path;
      RawCfaKind               kind;
      OpenClDemosaicNetVariant variant;
    };
    const Fixture fixtures[] = {
        {"Nikon D800E Bayer (full frame, OpenCL Neural)", BayerPath(), RawCfaKind::Bayer2x2,
         OpenClDemosaicNetVariant::Bayer},
        {"Fujifilm X-T5 X-Trans (full frame, OpenCL Neural)", XTransPath(), RawCfaKind::XTrans6x6,
         OpenClDemosaicNetVariant::XTrans},
    };

    for (const auto& fix : fixtures) {
#if ALCEDO_FULLFRAME_HAS_CUDA
      std::optional<std::vector<double>> cuda_samples;
      if (cuda_control) {
        // Alternating same-session order: CUDA control first, then OpenCL.
        std::cout << "\n--- CUDA control for " << fix.name << " ---\n";
        cuda_samples = TimeCudaControl(fix.path, warmup, iterations);
      }
#else
      if (cuda_control) {
        std::cout << "CUDA control requested but CUDA is not enabled in this build.\n";
      }
#endif

      auto run_modes = [&](const ProfileKind kind, const char* label) {
        std::cout << "\n--- " << label << " ---\n";
        auto stats =
            TimeFixtureOpenCl(fix.name, fix.path, fix.kind, fix.variant, warmup, iterations, kind);
#if ALCEDO_FULLFRAME_HAS_CUDA
        if (cuda_samples.has_value()) {
          stats.cuda_hot_ms      = *cuda_samples;
          stats.cuda_hot_mean_ms = Mean(stats.cuda_hot_ms);
          stats.has_cuda         = true;
        }
#endif
        PrintStats(stats);
        all_runs.push_back(std::move(stats));
      };

      if (do_compare) {
        run_modes(ProfileKind::WallOnly, "unprofiled wall");
        run_modes(ProfileKind::EventTimestamps, "event-profiled");
        // Residual agreement between the two series for this fixture.
        if (all_runs.size() >= 2) {
          const auto&  wall      = all_runs[all_runs.size() - 2];
          const auto&  event     = all_runs[all_runs.size() - 1];
          const double wall_mean = Mean(wall.hot_ms);
          const double event_wall =
              event.has_event_summary ? event.event_summary.wall_ms : Mean(event.hot_ms);
          const double residual = event_wall - wall_mean;
          std::cout << "\n  Compare residual (event_wall − unprofiled_wall): " << std::fixed
                    << std::setprecision(2) << residual << " ms ("
                    << (wall_mean > 0.0 ? 100.0 * residual / wall_mean : 0.0) << "% of wall)\n";
          if (event.has_event_summary) {
            std::cout << "  Material time explained by device_exec: "
                      << event.event_summary.device_exec_sum_ms << " ms; residual wall−device: "
                      << event.event_summary.residual_wall_minus_device_ms << " ms\n";
          }
        }
      } else {
        if (do_wall) {
          run_modes(ProfileKind::WallOnly, "unprofiled wall");
        }
        if (do_event) {
          run_modes(ProfileKind::EventTimestamps, "event-profiled");
        }
        if (do_boundary) {
          run_modes(ProfileKind::BoundaryDrain, "boundary drain");
        }
      }

#if ALCEDO_FULLFRAME_HAS_CUDA
      // Alternate fixture order contribution: after OpenCL, optional reverse CUDA re-check
      // is already covered by running CUDA first per fixture.
      (void)0;
#endif
    }

    const auto telem_after = QueryGpuTelemetry();
    if (telem_after.available) {
      std::cout << "\nGPU telemetry after: temp=" << telem_after.temperature_c
                << "C power=" << telem_after.power_w << "W sm=" << telem_after.sm_clock_mhz
                << "MHz mem=" << telem_after.mem_clock_mhz << "MHz pstate=" << telem_after.pstate
                << "\n";
    }

    std::cout << "\n--- Summary (hot mean, ms) ---\n";
    std::cout << std::fixed << std::setprecision(2);
    for (const auto& r : all_runs) {
      std::cout << "  " << r.name << " [";
      switch (r.profile_kind) {
        case ProfileKind::WallOnly:
          std::cout << "wall";
          break;
        case ProfileKind::EventTimestamps:
          std::cout << "event";
          break;
        case ProfileKind::BoundaryDrain:
          std::cout << "boundary";
          break;
      }
      std::cout << "]: " << Mean(r.hot_ms) << " ms";
      if (r.hot_counters_mean.h2d_bytes == 0 && r.hot_counters_mean.d2h_bytes == 0) {
        std::cout << "  (hot H2D/D2H=0)";
      } else {
        std::cout << "  (hot H2D=" << r.hot_counters_mean.h2d_bytes
                  << " D2H=" << r.hot_counters_mean.d2h_bytes << ")";
      }
      std::cout << "\n";
    }

    if (!json_path.empty()) {
      WriteJson(json_path, all_runs, caps, telem_before, telem_after, neural_build_options);
      std::cout << "Wrote JSON: " << json_path.string() << "\n";
    }
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
