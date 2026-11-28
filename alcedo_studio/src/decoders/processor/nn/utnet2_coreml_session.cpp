//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/nn/utnet2_coreml_session.hpp"

#include <onnxruntime_cxx_api.h>
#include <onnxruntime_session_options_config_keys.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "decoders/processor/nn/utnet2_onnx_graph.hpp"
#include "decoders/processor/nn/utnet2_tile_io.hpp"

namespace alcedo::nn {
namespace {

using Clock                                  = std::chrono::steady_clock;

// The free dimensions of the graph (DN2, export_fp16_onnx.py of the lab).
constexpr const char* kHeightDimension       = "height";
constexpr const char* kWidthDimension        = "width";
constexpr const char* kOutputHeightDimension = "output_height";
constexpr const char* kOutputWidthDimension  = "output_width";

/// The process-wide environment. It is not destroyed: sessions of other owners can end during
/// the static destruction of the process, after this function's statics.
auto                  ProcessEnv() -> Ort::Env& {
  static Ort::Env* env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "alcedo_ai_denoise");
  return *env;
}

auto SysctlText(const char* name) -> std::string {
  std::size_t size = 0;
  if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) {
    throw std::runtime_error(std::string("macOS: sysctl ") + name + " failed");
  }
  std::string value(size, '\0');
  if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) {
    throw std::runtime_error(std::string("macOS: sysctl ") + name + " failed");
  }
  value.resize(std::min(value.size(), value.find('\0')));
  return value;
}

auto PhysicalMemoryBytes() -> std::uint64_t {
  std::uint64_t bytes = 0;
  std::size_t   size  = sizeof(bytes);
  if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) != 0) {
    throw std::runtime_error("macOS: sysctl hw.memsize failed");
  }
  return bytes;
}

/// The percentage of the physical memory that is available (`kern.memorystatus_level`, the
/// "System-wide memory free percentage" of `memory_pressure`): free, inactive, purgeable, and
/// file-backed pages, which macOS gives to a new allocation without memory pressure.
auto AvailableMemoryPercent() -> std::uint64_t {
  std::uint32_t level = 0;
  std::size_t   size  = sizeof(level);
  if (sysctlbyname("kern.memorystatus_level", &level, &size, nullptr, 0) != 0) {
    throw std::runtime_error("macOS: sysctl kern.memorystatus_level failed");
  }
  return std::min<std::uint64_t>(level, 100);
}

auto SessionOptions(int width, int height, const std::filesystem::path& model_cache_folder)
    -> Ort::SessionOptions {
  Ort::SessionOptions options;
  options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  options.SetIntraOpNumThreads(4);
  options.SetInterOpNumThreads(1);
  options.AddConfigEntry(kOrtSessionOptionsConfigAllowIntraOpSpinning, "0");
  options.DisableCpuMemArena();
  options.AddFreeDimensionOverrideByName(kHeightDimension, height);
  options.AddFreeDimensionOverrideByName(kWidthDimension, width);
  options.AddFreeDimensionOverrideByName(kOutputHeightDimension,
                                         std::int64_t{UtNet2Spec::kOutputUpscale} * height);
  options.AddFreeDimensionOverrideByName(kOutputWidthDimension,
                                         std::int64_t{UtNet2Spec::kOutputUpscale} * width);
  std::unordered_map<std::string, std::string> provider{
      {"ModelFormat", "MLProgram"},
      {"MLComputeUnits", "CPUAndNeuralEngine"},
  };
  if (!model_cache_folder.empty()) {
    // The Core ML execution provider keys its cache on the graph and not on the free dimension
    // overrides: one folder for each tile shape, or a session loads the model of another shape.
    provider.emplace("ModelCacheDirectory",
                     UtNet2PathText(model_cache_folder /
                                    (std::to_string(width) + "x" + std::to_string(height))));
  }
  options.AppendExecutionProvider("CoreML", provider);
  // A node that Core ML does not take is a load error, not a CPU run (plan 2.4).
  options.AddConfigEntry(kOrtSessionOptionsDisableCPUEPFallback, "1");
  return options;
}

}  // namespace

struct UtNet2CoreMlSession::Impl {
  std::filesystem::path                       graph_path;
  std::filesystem::path                       model_cache_folder;
  float                                       output_scale = 1.0f;
  // The session of each tile shape (width, height), and the shapes that ran on their session.
  std::map<std::pair<int, int>, Ort::Session> sessions;
  std::set<std::pair<int, int>>               ran_shapes;
  int                                         largest_tile_side = 0;
  std::chrono::nanoseconds                    creation_time{};
  // The float16 tile input and output on the host, reused for each tile.
  std::vector<std::uint16_t>                  input_half;
  std::vector<std::uint16_t>                  output_half;

  /// Checks the graph with a CPU session that ends at once.
  void                                        CheckGraph() {
    if (!std::filesystem::is_regular_file(graph_path)) {
      throw std::runtime_error("UtNet2 graph " + UtNet2PathText(graph_path) + " does not exist");
    }
    try {
      Ort::SessionOptions options;
      options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_DISABLE_ALL);
      options.SetIntraOpNumThreads(1);
      Ort::Session session(ProcessEnv(), graph_path.c_str(), options);
      output_scale = CheckUtNet2Graph(session, graph_path);
    } catch (const Ort::Exception& error) {
      throw std::runtime_error("UtNet2 graph " + UtNet2PathText(graph_path) +
                                                                      ": ONNX Runtime: " + error.what());
    }
  }

  auto SessionFor(const PackedTileRect& tile) -> Ort::Session& {
    const auto key = std::make_pair(tile.width, tile.height);
    if (const auto found = sessions.find(key); found != sessions.end()) {
      return found->second;
    }
    if (!std::filesystem::is_regular_file(graph_path)) {
      throw std::runtime_error("UtNet2 graph " + UtNet2PathText(graph_path) + " does not exist");
    }
    const auto started = Clock::now();
    try {
      Ort::Session created(ProcessEnv(), graph_path.c_str(),
                           SessionOptions(tile.width, tile.height, model_cache_folder));
      const float  scale = CheckUtNet2Graph(created, graph_path);
      if (scale != output_scale) {
        throw std::runtime_error("UtNet2 graph " + UtNet2PathText(graph_path) +
                                 ": the file changed since the session checked it");
      }
      creation_time += Clock::now() - started;
      return sessions.emplace(key, std::move(created)).first->second;
    } catch (const Ort::Exception& error) {
      throw std::runtime_error("UtNet2 graph " + UtNet2PathText(graph_path) + ": " +
                               UtNet2TileText(tile) + ": ONNX Runtime: " + error.what());
    }
  }

  void RunTile(std::span<const float> input, const PackedTileRect& tile,
               const TileOutputCrop& crop) {
    auto&         session       = SessionFor(tile);
    constexpr int kUpscale      = UtNet2Spec::kOutputUpscale;
    const int     output_width  = kUpscale * tile.width;
    const int     output_height = kUpscale * tile.height;
    const auto    input_count =
        UtNet2Spec::kInputChannels * static_cast<std::size_t>(tile.width) * tile.height;
    const auto output_count =
        UtNet2Spec::kOutputChannels * static_cast<std::size_t>(output_width) * output_height;
    input_half.resize(input_count);
    output_half.resize(output_count);
    ConvertUtNet2TileInput(input, tile, input_half.data());

    const std::array<std::int64_t, 4> input_shape  = {1, UtNet2Spec::kInputChannels, tile.height,
                                                      tile.width};
    const std::array<std::int64_t, 4> output_shape = {1, UtNet2Spec::kOutputChannels, output_height,
                                                      output_width};
    try {
      static const Ort::MemoryInfo memory =
          Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);
      auto input_value = Ort::Value::CreateTensor(
          memory, input_half.data(), input_half.size() * sizeof(std::uint16_t), input_shape.data(),
          input_shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16);
      auto output_value = Ort::Value::CreateTensor(
          memory, output_half.data(), output_half.size() * sizeof(std::uint16_t),
          output_shape.data(), output_shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16);
      const char* input_names[]  = {UtNet2Spec::kInputName.data()};
      const char* output_names[] = {UtNet2Spec::kOutputName.data()};
      session.Run(Ort::RunOptions{nullptr}, input_names, &input_value, 1, output_names,
                  &output_value, 1);
    } catch (const Ort::Exception& error) {
      throw std::runtime_error(UtNet2TileText(tile) + ": ONNX Runtime: " + error.what());
    }

    const auto row_bytes   = static_cast<std::size_t>(output_width) * sizeof(std::uint16_t);
    const auto plane_bytes = row_bytes * output_height;
    WriteUtNet2OutputCrop(
        UtNet2Float16OutputRows{
            .first_row = reinterpret_cast<const std::uint8_t*>(output_half.data()) +
                         static_cast<std::size_t>(crop.source_y) * row_bytes,
            .row_bytes   = row_bytes,
            .plane_bytes = plane_bytes},
        tile, crop, output_scale);
  }
};

auto CoreMlModelCacheRoot() -> std::filesystem::path {
  std::size_t length = confstr(_CS_DARWIN_USER_CACHE_DIR, nullptr, 0);
  if (length == 0) {
    throw std::runtime_error("macOS: confstr _CS_DARWIN_USER_CACHE_DIR failed");
  }
  std::string folder(length, '\0');
  confstr(_CS_DARWIN_USER_CACHE_DIR, folder.data(), length);
  folder.resize(length - 1);
  return std::filesystem::path(folder) / "alcedo-studio" / "ai-denoise-coreml";
}

auto PrepareCoreMlModelCache(const std::filesystem::path& root, std::string_view model_version)
    -> std::filesystem::path {
  // The version is a folder name beside folders that this function removes.
  if (model_version.size() != 64 ||
      model_version.find_first_not_of("0123456789abcdef") != std::string_view::npos) {
    throw std::runtime_error("AI Denoise: the model version `" + std::string(model_version) +
                             "` is not 64 lower-case hex digits");
  }
  const auto      folder = root / std::string(model_version);
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  if (error) {
    throw std::runtime_error("AI Denoise: the Core ML model cache " + UtNet2PathText(folder) +
                             " cannot be created: " + error.message());
  }
  for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
    if (entry.path().filename() != folder.filename()) {
      std::error_code remove_error;
      std::filesystem::remove_all(entry.path(), remove_error);
    }
  }
  return folder;
}

auto CoreMlDeviceKey() -> std::string {
  return SysctlText("machdep.cpu.brand_string") + " Neural Engine";
}

void LoadCoreMlRuntime() {
  // ALCEDO_ONNXRUNTIME_MACOS_VERSION: the version of the package of the headers
  // (fetch_onnxruntime_macos.cmake).
  const std::string version = Ort::GetVersionString();
  if (version != ALCEDO_ONNXRUNTIME_MACOS_VERSION) {
    throw std::runtime_error("ONNX Runtime: the library has version " + version + ", the headers " +
                             ALCEDO_ONNXRUNTIME_MACOS_VERSION);
  }
  const auto providers = Ort::GetAvailableProviders();
  if (std::find(providers.begin(), providers.end(), "CoreMLExecutionProvider") == providers.end()) {
    throw std::runtime_error("ONNX Runtime " + version +
                             ": the Core ML execution provider is absent");
  }
}

UtNet2CoreMlSession::UtNet2CoreMlSession(std::filesystem::path graph_path,
                                         std::filesystem::path model_cache_folder)
    : impl_(std::make_unique<Impl>()) {
  impl_->graph_path         = std::move(graph_path);
  impl_->model_cache_folder = std::move(model_cache_folder);
  impl_->CheckGraph();
}

UtNet2CoreMlSession::~UtNet2CoreMlSession()                              = default;
UtNet2CoreMlSession::UtNet2CoreMlSession(UtNet2CoreMlSession&&) noexcept = default;
auto UtNet2CoreMlSession::operator=(UtNet2CoreMlSession&&) noexcept
    -> UtNet2CoreMlSession& = default;

void UtNet2CoreMlSession::Run(std::span<const float> input, const PackedTileRect& tile,
                              const TileOutputCrop& crop) {
  CheckUtNet2TileRun(input, tile, crop);
  impl_->RunTile(input, tile, crop);
  impl_->ran_shapes.emplace(tile.width, tile.height);
  impl_->largest_tile_side = std::max({impl_->largest_tile_side, tile.width, tile.height});
}

void UtNet2CoreMlSession::Run(std::span<const float> input, const PackedTileRect& tile,
                              std::span<float> output) {
  constexpr int kUpscale = UtNet2Spec::kOutputUpscale;
  Run(input, tile,
      TileOutputCrop{.source_x      = 0,
                     .source_y      = 0,
                     .width         = kUpscale * tile.width,
                     .height        = kUpscale * tile.height,
                     .destination   = {output, kUpscale * tile.width, kUpscale * tile.height},
                     .destination_x = 0,
                     .destination_y = 0});
}

auto UtNet2CoreMlSession::IsReadyFor(int width, int height) const -> bool {
  return impl_->ran_shapes.contains({width, height});
}

auto UtNet2CoreMlSession::DeviceMemory() const -> DeviceMemoryState {
  const auto        physical  = PhysicalMemoryBytes();
  const auto        available = physical / 100 * AvailableMemoryPercent();
  DeviceMemoryState state{.budget_bytes = physical, .usage_bytes = physical - available};
  for (const auto& entry : UtNet2Spec::kCoreMlTileMemory) {
    if (!impl_->sessions.empty() && entry.tile_size <= impl_->largest_tile_side) {
      state.held_bytes = std::max(state.held_bytes, entry.device_bytes);
    }
  }
  return state;
}

auto UtNet2CoreMlSession::TileMemory() const -> std::span<const UtNet2TileMemory> {
  return UtNet2Spec::kCoreMlTileMemory;
}

void UtNet2CoreMlSession::Release() {
  impl_->sessions.clear();
  impl_->ran_shapes.clear();
  impl_->largest_tile_side = 0;
  impl_->creation_time     = {};
  impl_->input_half        = {};
  impl_->output_half       = {};
}

auto UtNet2CoreMlSession::IsLoaded() const -> bool { return !impl_->sessions.empty(); }

auto UtNet2CoreMlSession::SessionCount() const -> int {
  return static_cast<int>(impl_->sessions.size());
}

auto UtNet2CoreMlSession::OutputScale() const -> float { return impl_->output_scale; }

auto UtNet2CoreMlSession::GraphPath() const -> const std::filesystem::path& {
  return impl_->graph_path;
}

auto UtNet2CoreMlSession::SessionCreationTime() const -> std::chrono::nanoseconds {
  return impl_->creation_time;
}

}  // namespace alcedo::nn
