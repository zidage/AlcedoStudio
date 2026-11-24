//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/nn/utnet2_directml_session.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// ORT_API_MANUAL_INIT comes from the alcedo::onnxruntime_directml target: onnxruntime.dll is
// delay-loaded, and OnnxRuntimeApi() loads it at the first session.
#include <dml_provider_factory.h>
#include <onnxruntime_cxx_api.h>
#include <onnxruntime_session_options_config_keys.h>

#include <opencv2/core.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "decoders/processor/nn/utnet2_specs.hpp"

namespace alcedo::nn {
namespace {

auto PathText(const std::filesystem::path& path) -> std::string {
  const auto text = path.u8string();
  return std::string(text.begin(), text.end());
}

auto TileText(const PackedTileRect& tile) -> std::string {
  std::ostringstream text;
  text << "UtNet2 tile at (" << tile.x << ", " << tile.y << ") of " << tile.width << " x "
       << tile.height << " packed px";
  return text.str();
}

auto ModulePath(HMODULE module) -> std::string {
  std::wstring buffer(MAX_PATH, L'\0');
  const DWORD  length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
  buffer.resize(length);
  return PathText(std::filesystem::path(buffer));
}

/// Loads onnxruntime.dll (the folder of the executable first, then System32) and initializes the
/// C++ API. The delay-load helper of the linker then resolves to the loaded module. A
/// System32 copy of Windows (an older version) fails the API version check with its path.
auto OnnxRuntimeApi() -> const OrtApi& {
  static const OrtApi* api = [] {
    HMODULE module = LoadLibraryExW(L"onnxruntime.dll", nullptr,
                                    LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
      throw std::runtime_error("ONNX Runtime: onnxruntime.dll failed to load (Windows error " +
                               std::to_string(GetLastError()) + ")");
    }
    const OrtApiBase* base   = OrtGetApiBase();
    const OrtApi*     result = base->GetApi(ORT_API_VERSION);
    if (result == nullptr) {
      throw std::runtime_error("ONNX Runtime: " + ModulePath(module) + " (version " +
                               base->GetVersionString() + ") does not provide API version " +
                               std::to_string(ORT_API_VERSION));
    }
    Ort::InitApi(result);
    return result;
  }();
  return *api;
}

/// The process-wide environment. It is not destroyed: sessions of other owners can end during
/// the static destruction of the process, after this function's statics.
auto ProcessEnv() -> Ort::Env& {
  static Ort::Env* env = [] {
    OnnxRuntimeApi();
    return new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "alcedo_ai_denoise");
  }();
  return *env;
}

auto DmlApi() -> const OrtDmlApi& {
  const void* api = nullptr;
  Ort::ThrowOnError(OnnxRuntimeApi().GetExecutionProviderApi("DML", ORT_API_VERSION, &api));
  if (api == nullptr) {
    throw std::runtime_error("ONNX Runtime: the DirectML execution provider API is absent");
  }
  return *static_cast<const OrtDmlApi*>(api);
}

auto SessionOptions() -> Ort::SessionOptions {
  Ort::SessionOptions options;
  options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  // Required by the DirectML execution provider.
  options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
  options.DisableMemPattern();
  options.DisableCpuMemArena();
  options.SetIntraOpNumThreads(4);
  options.SetInterOpNumThreads(1);
  options.AddConfigEntry(kOrtSessionOptionsConfigAllowIntraOpSpinning, "0");
  // A node that DirectML cannot run is a load error, not a CPU run (plan 2.4).
  options.AddConfigEntry(kOrtSessionOptionsDisableCPUEPFallback, "1");
  OrtDmlDeviceOptions device{};
  device.Preference = OrtDmlPerformancePreference::HighPerformance;
  device.Filter     = OrtDmlDeviceFilter::Gpu;
  Ort::ThrowOnError(DmlApi().SessionOptionsAppendExecutionProvider_DML2(options, &device));
  return options;
}

void RequireTensor(const Ort::TypeInfo& info, std::string_view role, std::string_view name,
                   std::string_view expected_name, int channels,
                   const std::filesystem::path& path) {
  const auto prefix = "UtNet2 graph " + PathText(path) + ": ";
  if (name != expected_name) {
    throw std::runtime_error(prefix + std::string(role) + " is `" + std::string(name) +
                             "`, expected `" + std::string(expected_name) + "`");
  }
  const auto tensor = info.GetTensorTypeAndShapeInfo();
  if (tensor.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
    throw std::runtime_error(prefix + std::string(role) + " `" + std::string(name) +
                             "` is not float16 (element type " +
                             std::to_string(static_cast<int>(tensor.GetElementType())) + ")");
  }
  const auto shape = tensor.GetShape();
  if (shape.size() != 4 || shape[1] != channels) {
    throw std::runtime_error(prefix + std::string(role) + " `" + std::string(name) +
                             "` is not (1, " + std::to_string(channels) + ", height, width)");
  }
}

auto RequireMetadata(const Ort::ModelMetadata& metadata, std::string_view key,
                     const std::filesystem::path& path) -> std::string {
  Ort::AllocatorWithDefaultOptions allocator;
  const auto value = metadata.LookupCustomMetadataMapAllocated(std::string(key).c_str(), allocator);
  if (!value) {
    throw std::runtime_error("UtNet2 graph " + PathText(path) + ": metadata `" +
                             std::string(key) + "` is absent");
  }
  return std::string(value.get());
}

void RequireMetadataValue(const Ort::ModelMetadata& metadata, std::string_view key,
                          std::string_view expected, const std::filesystem::path& path) {
  const auto value = RequireMetadata(metadata, key, path);
  if (value != expected) {
    throw std::runtime_error("UtNet2 graph " + PathText(path) + ": metadata `" +
                             std::string(key) + "` is `" + value + "`, expected `" +
                             std::string(expected) + "`");
  }
}

/// Checks the graph of `session` against UtNet2Spec; returns its `output_scale`.
auto CheckGraph(Ort::Session& session, const std::filesystem::path& path) -> float {
  const auto prefix = "UtNet2 graph " + PathText(path) + ": ";
  if (session.GetInputCount() != 1 || session.GetOutputCount() != 1) {
    throw std::runtime_error(prefix + "the graph has " + std::to_string(session.GetInputCount()) +
                             " inputs and " + std::to_string(session.GetOutputCount()) +
                             " outputs, expected 1 and 1");
  }
  Ort::AllocatorWithDefaultOptions allocator;
  RequireTensor(session.GetInputTypeInfo(0), "input", session.GetInputNameAllocated(0, allocator).get(),
                UtNet2Spec::kInputName, UtNet2Spec::kInputChannels, path);
  RequireTensor(session.GetOutputTypeInfo(0), "output",
                session.GetOutputNameAllocated(0, allocator).get(), UtNet2Spec::kOutputName,
                UtNet2Spec::kOutputChannels, path);

  const auto metadata = session.GetModelMetadata();
  RequireMetadataValue(metadata, UtNet2Spec::kMetadataArchitecture, UtNet2Spec::kArchitecture, path);
  RequireMetadataValue(metadata, UtNet2Spec::kMetadataPrecision, UtNet2Spec::kPrecision, path);
  RequireMetadataValue(metadata, UtNet2Spec::kMetadataInputPreparationVersion,
                       UtNet2Spec::kInputPreparationVersion, path);
  const auto sha256 = RequireMetadata(metadata, UtNet2Spec::kMetadataWeightsSha256, path);
  if (sha256.size() != 64 ||
      sha256.find_first_not_of("0123456789abcdef") != std::string::npos) {
    throw std::runtime_error(prefix + "metadata `" + std::string(UtNet2Spec::kMetadataWeightsSha256) +
                             "` is `" + sha256 + "`, expected 64 lower-case hex digits");
  }
  const auto scale_text = RequireMetadata(metadata, UtNet2Spec::kMetadataOutputScale, path);
  double     scale      = 0.0;
  const auto parsed = std::from_chars(scale_text.data(), scale_text.data() + scale_text.size(), scale);
  if (parsed.ec != std::errc{} || parsed.ptr != scale_text.data() + scale_text.size() ||
      !std::isfinite(scale) || scale <= 0.0) {
    throw std::runtime_error(prefix + "metadata `" + std::string(UtNet2Spec::kMetadataOutputScale) +
                             "` is `" + scale_text + "`, expected a finite number above 0");
  }
  return static_cast<float>(scale);
}

}  // namespace

struct UtNet2DirectMlSession::Impl {
  std::filesystem::path       graph_path;
  float                       output_scale = 1.0f;
  std::optional<Ort::Session> session;
  // float16 host buffers of the last tile size, reused for each tile of that size.
  std::vector<std::uint16_t>  input_half;
  std::vector<std::uint16_t>  output_half;

  void Load() {
    if (!std::filesystem::is_regular_file(graph_path)) {
      throw std::runtime_error("UtNet2 graph " + PathText(graph_path) + " does not exist");
    }
    try {
      auto& env     = ProcessEnv();
      auto  options = SessionOptions();
      Ort::Session created(env, graph_path.c_str(), options);
      output_scale = CheckGraph(created, graph_path);
      session.emplace(std::move(created));
    } catch (const Ort::Exception& error) {
      throw std::runtime_error("UtNet2 graph " + PathText(graph_path) +
                               ": ONNX Runtime: " + error.what());
    }
  }
};

UtNet2DirectMlSession::UtNet2DirectMlSession(std::filesystem::path graph_path)
    : impl_(std::make_unique<Impl>()) {
  impl_->graph_path = std::move(graph_path);
  impl_->Load();
}

UtNet2DirectMlSession::~UtNet2DirectMlSession()                                      = default;
UtNet2DirectMlSession::UtNet2DirectMlSession(UtNet2DirectMlSession&&) noexcept       = default;
auto UtNet2DirectMlSession::operator=(UtNet2DirectMlSession&&) noexcept
    -> UtNet2DirectMlSession& = default;

void UtNet2DirectMlSession::Run(std::span<const float> input, const PackedTileRect& tile,
                                std::span<float> output) {
  constexpr int kAlignment = UtNet2Spec::kSizeAlignment;
  if (tile.width <= 0 || tile.height <= 0 || tile.width % kAlignment != 0 ||
      tile.height % kAlignment != 0) {
    throw std::runtime_error(TileText(tile) + ": the height and the width must be positive " +
                             "multiples of " + std::to_string(kAlignment));
  }
  const auto pixels       = static_cast<std::size_t>(tile.width) * tile.height;
  const auto input_count  = UtNet2Spec::kInputChannels * pixels;
  const auto output_count = UtNet2Spec::kOutputChannels * UtNet2Spec::kOutputUpscale *
                            UtNet2Spec::kOutputUpscale * pixels;
  if (input.size() != input_count || output.size() != output_count) {
    throw std::runtime_error(TileText(tile) + ": the input has " + std::to_string(input.size()) +
                             " values and the output " + std::to_string(output.size()) +
                             ", expected " + std::to_string(input_count) + " and " +
                             std::to_string(output_count));
  }
  if (!impl_->session) {
    impl_->Load();
  }

  impl_->input_half.resize(input_count);
  impl_->output_half.resize(output_count);
  const cv::Mat input_float(1, static_cast<int>(input_count), CV_32F,
                            const_cast<float*>(input.data()));
  cv::Mat       input_half(1, static_cast<int>(input_count), CV_16F, impl_->input_half.data());
  input_float.convertTo(input_half, CV_16F);

  const std::array<std::int64_t, 4> input_shape  = {1, UtNet2Spec::kInputChannels, tile.height,
                                                    tile.width};
  const std::array<std::int64_t, 4> output_shape = {
      1, UtNet2Spec::kOutputChannels, std::int64_t{UtNet2Spec::kOutputUpscale} * tile.height,
      std::int64_t{UtNet2Spec::kOutputUpscale} * tile.width};
  try {
    const auto memory = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);
    auto input_value  = Ort::Value::CreateTensor(
        memory, impl_->input_half.data(), input_count * sizeof(std::uint16_t), input_shape.data(),
        input_shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16);
    auto output_value = Ort::Value::CreateTensor(
        memory, impl_->output_half.data(), output_count * sizeof(std::uint16_t),
        output_shape.data(), output_shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16);
    const char* input_names[]  = {UtNet2Spec::kInputName.data()};
    const char* output_names[] = {UtNet2Spec::kOutputName.data()};
    impl_->session->Run(Ort::RunOptions{nullptr}, input_names, &input_value, 1, output_names,
                        &output_value, 1);
  } catch (const Ort::Exception& error) {
    throw std::runtime_error(TileText(tile) + ": ONNX Runtime: " + error.what());
  }

  const cv::Mat output_half(1, static_cast<int>(output_count), CV_16F, impl_->output_half.data());
  cv::Mat       output_float(1, static_cast<int>(output_count), CV_32F, output.data());
  output_half.convertTo(output_float, CV_32F, impl_->output_scale);
  cv::Point first_invalid;
  if (!cv::checkRange(output_float, true, &first_invalid)) {
    const auto index     = static_cast<std::size_t>(first_invalid.x);
    const auto plane     = output_count / UtNet2Spec::kOutputChannels;
    const auto out_width = static_cast<std::size_t>(UtNet2Spec::kOutputUpscale) * tile.width;
    std::ostringstream text;
    text << TileText(tile) << ": the output has a value that is not finite (FP16 overflow) at "
         << "channel " << index / plane << ", output row " << (index % plane) / out_width
         << ", output column " << index % out_width;
    throw std::runtime_error(text.str());
  }
}

void UtNet2DirectMlSession::Release() {
  impl_->session.reset();
  impl_->input_half  = {};
  impl_->output_half = {};
}

auto UtNet2DirectMlSession::IsLoaded() const -> bool { return impl_->session.has_value(); }

auto UtNet2DirectMlSession::OutputScale() const -> float { return impl_->output_scale; }

auto UtNet2DirectMlSession::GraphPath() const -> const std::filesystem::path& {
  return impl_->graph_path;
}

}  // namespace alcedo::nn
