//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The photo timing tool of AI Denoise (integration plan, DN3): the time of the tile loop
// (DenoiseBayerMosaic) with UtNet2DirectMlSession on full photos.
//
// Input: the folder of alcedo-denoise-lab scripts/export/write_raw_inputs.py (`manifest.txt` and
// the packed photos in float32, input preparation 1) and the FP16 graph.
//
//   UtNet2PhotoTimingTool --graph <fp16 onnx> --inputs <folder> [--photos 24mp 36mp 45mp]
//       [--repeats 3] [--tile auto|768|1024|1536] [--adapter high-performance|minimum-power]
//       [--no-slow-device-check] [--per-tile]
//
// For each photo: one run that is not measured (the first run of the session compiles the graph;
// a photo after the first one has a ready session), then `--repeats` runs with 1 s between them.
// Each run prints the tile size, the tile count, the time of the tile loop, the sum of the `Run`
// times, the sum of the device times (Direct3D 12 timestamps, UtNet2DirectMlSession::
// LastRunDeviceTime), the local video memory of the process and its growth since before the
// session, and the GPU temperature and SM clock after the run (NVML, NVIDIA adapters only).
// A run that the slow-device check stops prints the tile and the projected time.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "decoders/processor/nn/denoise_errors.hpp"
#include "decoders/processor/nn/utnet2_directml_session.hpp"
#include "decoders/processor/nn/utnet2_tile_loop.hpp"

namespace {

using alcedo::nn::DenoiseBayerMosaic;
using alcedo::nn::DenoiseDeviceTooSlowError;
using alcedo::nn::DirectMlAdapterPreference;
using alcedo::nn::PackedMosaicView;
using alcedo::nn::RgbPlanesView;
using alcedo::nn::UtNet2DirectMlSession;
using alcedo::nn::UtNet2TileLoopOptions;
using alcedo::nn::UtNet2TileResult;

struct Arguments {
  std::filesystem::path     graph;
  std::filesystem::path     inputs;
  std::vector<std::string>  photos            = {"24mp", "36mp", "45mp"};
  int                       repeats           = 3;
  int                       tile              = 0;
  DirectMlAdapterPreference adapter           = DirectMlAdapterPreference::HighPerformance;
  bool                      slow_device_check = true;
  bool                      per_tile          = false;
};

auto ParseArguments(int argc, char** argv) -> Arguments {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string name  = argv[index];
    const auto        value = [&]() -> std::string {
      if (index + 1 >= argc) {
        throw std::invalid_argument(name + " needs a value");
      }
      return argv[++index];
    };
    if (name == "--graph") {
      arguments.graph = value();
    } else if (name == "--inputs") {
      arguments.inputs = value();
    } else if (name == "--photos") {
      arguments.photos.clear();
      while (index + 1 < argc && std::string(argv[index + 1]).rfind("--", 0) != 0) {
        arguments.photos.emplace_back(argv[++index]);
      }
    } else if (name == "--repeats") {
      arguments.repeats = std::stoi(value());
    } else if (name == "--tile") {
      const auto text = value();
      arguments.tile  = text == "auto" ? 0 : std::stoi(text);
    } else if (name == "--adapter") {
      const auto text = value();
      if (text == "minimum-power") {
        arguments.adapter = DirectMlAdapterPreference::MinimumPower;
      } else if (text != "high-performance") {
        throw std::invalid_argument("--adapter " + text);
      }
    } else if (name == "--no-slow-device-check") {
      arguments.slow_device_check = false;
    } else if (name == "--per-tile") {
      arguments.per_tile = true;
    } else {
      throw std::invalid_argument("unknown argument " + name);
    }
  }
  if (arguments.graph.empty() || arguments.inputs.empty()) {
    throw std::invalid_argument("--graph and --inputs are required");
  }
  return arguments;
}

struct PackedPhoto {
  int                width  = 0;
  int                height = 0;
  std::vector<float> data;
};

auto LoadPhoto(const std::filesystem::path& folder, const std::string& name) -> PackedPhoto {
  std::ifstream manifest(folder / "manifest.txt");
  std::string   line;
  while (std::getline(manifest, line)) {
    std::istringstream fields(line);
    std::string        entry, kind, file;
    int                channels = 0, height = 0, width = 0;
    fields >> entry >> kind >> channels >> height >> width >> file;
    if (entry != name || kind != "photo") {
      continue;
    }
    PackedPhoto   photo{width, height, std::vector<float>(std::size_t{4} * width * height)};
    std::ifstream stream(folder / file, std::ios::binary);
    stream.read(reinterpret_cast<char*>(photo.data.data()),
                static_cast<std::streamsize>(photo.data.size() * sizeof(float)));
    if (channels != 4 || !stream) {
      throw std::runtime_error("photo " + name + ": " + (folder / file).string() +
                               " is not a (4, h, w) float32 file");
    }
    return photo;
  }
  throw std::runtime_error("photo " + name + " is not in " + (folder / "manifest.txt").string());
}

/// The video memory of the process in `segment` on the adapter of `preference` (the adapter that
/// the session selects).
auto ProcessVideoMemory(DirectMlAdapterPreference preference,
                        DXGI_MEMORY_SEGMENT_GROUP segment = DXGI_MEMORY_SEGMENT_GROUP_LOCAL)
    -> std::uint64_t {
  Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
    return 0;
  }
  const auto gpu_preference = preference == DirectMlAdapterPreference::MinimumPower
                                  ? DXGI_GPU_PREFERENCE_MINIMUM_POWER
                                  : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
  for (UINT index = 0;; ++index) {
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
    if (factory->EnumAdapterByGpuPreference(index, gpu_preference, IID_PPV_ARGS(&adapter)) ==
        DXGI_ERROR_NOT_FOUND) {
      return 0;
    }
    DXGI_ADAPTER_DESC1 description{};
    if (adapter == nullptr || FAILED(adapter->GetDesc1(&description)) ||
        (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
      continue;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    adapter->QueryVideoMemoryInfo(0, segment, &info);
    return info.CurrentUsage;
  }
}

/// GPU temperature and SM clock through nvml.dll of the NVIDIA driver (device 0).
class NvmlReader {
 public:
  NvmlReader() {
    module_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module_ == nullptr) {
      return;
    }
    const auto init   = Function<int (*)()>("nvmlInit_v2");
    const auto handle = Function<int (*)(unsigned, void**)>("nvmlDeviceGetHandleByIndex_v2");
    temperature_      = Function<int (*)(void*, int, unsigned*)>("nvmlDeviceGetTemperature");
    clock_            = Function<int (*)(void*, int, unsigned*)>("nvmlDeviceGetClockInfo");
    available_        = init != nullptr && handle != nullptr && temperature_ != nullptr &&
                 clock_ != nullptr && init() == 0 && handle(0, &device_) == 0;
  }

  [[nodiscard]] auto Text() const -> std::string {
    if (!available_) {
      return "\"gpu\": null";
    }
    unsigned temperature = 0;
    unsigned clock       = 0;
    temperature_(device_, 0, &temperature);  // NVML_TEMPERATURE_GPU
    clock_(device_, 1, &clock);              // NVML_CLOCK_SM
    return "\"gpu_temperature_c\": " + std::to_string(temperature) +
           ", \"gpu_sm_clock_mhz\": " + std::to_string(clock);
  }

 private:
  template <typename T>
  auto Function(const char* name) -> T {
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module_, name)));
  }

  HMODULE module_                            = nullptr;
  void*   device_                            = nullptr;
  int (*temperature_)(void*, int, unsigned*) = nullptr;
  int (*clock_)(void*, int, unsigned*)       = nullptr;
  bool available_                            = false;
};

auto Seconds(std::chrono::nanoseconds time) -> double {
  return std::chrono::duration<double>(time).count();
}

auto Mebibytes(std::uint64_t bytes) -> double { return static_cast<double>(bytes) / (1 << 20); }

void RunPhoto(UtNet2DirectMlSession& session, const Arguments& arguments, const std::string& name,
              std::uint64_t usage_before_session, const NvmlReader& nvml) {
  const auto             photo = LoadPhoto(arguments.inputs, name);
  std::vector<float>     output(std::size_t{3} * 4 * photo.width * photo.height);
  const PackedMosaicView mosaic{photo.data, photo.width, photo.height};
  const RgbPlanesView    image{output, 2 * photo.width, 2 * photo.height};
  UtNet2TileLoopOptions  options;
  options.tile_size = arguments.tile;
  if (!arguments.slow_device_check) {
    options.slow_device_limit_seconds_per_megapixel = std::numeric_limits<double>::infinity();
  }
  const double megapixels = 4.0 * photo.width * photo.height / 1e6;

  for (int repeat = -1; repeat < arguments.repeats; ++repeat) {
    if (repeat >= 0) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::chrono::nanoseconds              device_time{};
    std::ostringstream                    tiles_text;
    std::chrono::steady_clock::time_point last    = std::chrono::steady_clock::now();
    const auto                            on_tile = [&](const UtNet2TileResult& tile) {
      const auto now = std::chrono::steady_clock::now();
      device_time += session.LastRunDeviceTime();
      if (arguments.per_tile) {
        tiles_text << (tile.index == 0 ? "" : ", ") << "[" << tile.input.width << ", "
                   << tile.input.height << ", " << Seconds(now - last) << ", "
                   << Seconds(session.LastRunDeviceTime()) << "]";
      }
      last = now;
    };
    const auto started = std::chrono::steady_clock::now();
    std::cout.precision(4);
    try {
      const auto record = DenoiseBayerMosaic(
          session, mosaic, image, on_tile, {}, [] { return std::chrono::steady_clock::now(); },
          options);
      const auto memory = session.DeviceMemory();
      std::cout << "{\"photo\": \"" << name << "\", \"megapixels\": " << megapixels
                << ", \"repeat\": " << repeat << ", \"tile\": " << record.tile_size
                << ", \"tiles\": " << record.tile_count
                << ", \"computed_packed_megapixels\": " << record.computed_packed_pixels / 1e6
                << ", \"loop_s\": " << Seconds(record.loop_time)
                << ", \"run_s\": " << Seconds(record.run_time)
                << ", \"device_s\": " << Seconds(device_time)
                << ", \"measured_tiles\": " << record.measured_tiles
                << ", \"process_video_mib\": " << Mebibytes(memory.usage_bytes)
                << ", \"growth_since_before_session_mib\": "
                << Mebibytes(memory.usage_bytes - usage_before_session)
                << ", \"budget_mib\": " << Mebibytes(memory.budget_bytes)
                << ", \"adapter_local_mib\": " << Mebibytes(memory.adapter_local_bytes)
                << ", \"adapter_local_usage_mib\": " << Mebibytes(memory.adapter_local_usage_bytes)
                << ", \"process_non_local_mib\": "
                << Mebibytes(
                       ProcessVideoMemory(arguments.adapter, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL))
                << ", " << nvml.Text();
    } catch (const DenoiseDeviceTooSlowError& error) {
      std::cout << "{\"photo\": \"" << name << "\", \"megapixels\": " << megapixels
                << ", \"repeat\": " << repeat << ", \"slow_device_error\": \"" << error.what()
                << "\", \"tiles_run\": " << error.TilesRun()
                << ", \"tile_count\": " << error.TileCount()
                << ", \"projected_s\": " << error.ProjectedSeconds()
                << ", \"limit_s\": " << error.LimitSeconds()
                << ", \"time_to_stop_s\": " << Seconds(std::chrono::steady_clock::now() - started)
                << ", \"device_s\": " << Seconds(device_time);
      const auto memory = session.DeviceMemory();
      std::cout << ", \"process_video_mib\": " << Mebibytes(memory.usage_bytes)
                << ", \"budget_mib\": " << Mebibytes(memory.budget_bytes)
                << ", \"adapter_local_usage_mib\": " << Mebibytes(memory.adapter_local_usage_bytes)
                << ", \"process_non_local_mib\": "
                << Mebibytes(
                       ProcessVideoMemory(arguments.adapter, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL))
                << ", " << nvml.Text();
    }
    if (arguments.per_tile) {
      std::cout << ", \"tiles_width_height_run_s_device_s\": [" << tiles_text.str() << "]";
    }
    std::cout << "}" << std::endl;
  }
}

}  // namespace

auto main(int argc, char** argv) -> int {
  try {
    const auto            arguments = ParseArguments(argc, argv);
    const auto            before    = ProcessVideoMemory(arguments.adapter);
    const auto            created   = std::chrono::steady_clock::now();
    UtNet2DirectMlSession session(arguments.graph, arguments.adapter);
    std::cout << "{\"adapter\": \"" << session.AdapterName() << "\", \"session_creation_s\": "
              << Seconds(std::chrono::steady_clock::now() - created)
              << ", \"process_video_before_session_mib\": " << Mebibytes(before) << "}"
              << std::endl;
    const NvmlReader nvml;
    for (const auto& photo : arguments.photos) {
      RunPhoto(session, arguments, photo, before, nvml);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << std::endl;
    return 1;
  }
}
