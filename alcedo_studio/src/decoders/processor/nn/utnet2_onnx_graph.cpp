//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/nn/utnet2_onnx_graph.hpp"

#include <cmath>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include "decoders/processor/nn/utnet2_specs.hpp"

namespace alcedo::nn {
namespace {

void RequireTensor(const Ort::TypeInfo& info, std::string_view role, std::string_view name,
                   std::string_view expected_name, int channels,
                   const std::filesystem::path& path) {
  const auto prefix = "UtNet2 graph " + UtNet2PathText(path) + ": ";
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
    throw std::runtime_error("UtNet2 graph " + UtNet2PathText(path) + ": metadata `" +
                             std::string(key) + "` is absent");
  }
  return std::string(value.get());
}

void RequireMetadataValue(const Ort::ModelMetadata& metadata, std::string_view key,
                          std::string_view expected, const std::filesystem::path& path) {
  const auto value = RequireMetadata(metadata, key, path);
  if (value != expected) {
    throw std::runtime_error("UtNet2 graph " + UtNet2PathText(path) + ": metadata `" +
                             std::string(key) + "` is `" + value + "`, expected `" +
                             std::string(expected) + "`");
  }
}

}  // namespace

auto UtNet2PathText(const std::filesystem::path& path) -> std::string {
  const auto text = path.u8string();
  return std::string(text.begin(), text.end());
}

auto CheckUtNet2Graph(Ort::Session& session, const std::filesystem::path& path) -> float {
  const auto prefix = "UtNet2 graph " + UtNet2PathText(path) + ": ";
  if (session.GetInputCount() != 1 || session.GetOutputCount() != 1) {
    throw std::runtime_error(prefix + "the graph has " + std::to_string(session.GetInputCount()) +
                             " inputs and " + std::to_string(session.GetOutputCount()) +
                             " outputs, expected 1 and 1");
  }
  Ort::AllocatorWithDefaultOptions allocator;
  RequireTensor(session.GetInputTypeInfo(0), "input",
                session.GetInputNameAllocated(0, allocator).get(), UtNet2Spec::kInputName,
                UtNet2Spec::kInputChannels, path);
  RequireTensor(session.GetOutputTypeInfo(0), "output",
                session.GetOutputNameAllocated(0, allocator).get(), UtNet2Spec::kOutputName,
                UtNet2Spec::kOutputChannels, path);

  const auto metadata = session.GetModelMetadata();
  RequireMetadataValue(metadata, UtNet2Spec::kMetadataArchitecture, UtNet2Spec::kArchitecture,
                       path);
  RequireMetadataValue(metadata, UtNet2Spec::kMetadataPrecision, UtNet2Spec::kPrecision, path);
  RequireMetadataValue(metadata, UtNet2Spec::kMetadataInputPreparationVersion,
                       UtNet2Spec::kInputPreparationVersion, path);
  const auto sha256 = RequireMetadata(metadata, UtNet2Spec::kMetadataWeightsSha256, path);
  if (sha256.size() != 64 || sha256.find_first_not_of("0123456789abcdef") != std::string::npos) {
    throw std::runtime_error(prefix + "metadata `" +
                             std::string(UtNet2Spec::kMetadataWeightsSha256) + "` is `" + sha256 +
                             "`, expected 64 lower-case hex digits");
  }
  const auto         scale_text = RequireMetadata(metadata, UtNet2Spec::kMetadataOutputScale, path);
  // std::from_chars of a double needs macOS 26; the classic locale reads the same text.
  double             scale      = 0.0;
  std::istringstream scale_stream(scale_text);
  scale_stream.imbue(std::locale::classic());
  scale_stream >> scale;
  if (scale_stream.fail() || !scale_stream.eof() || !std::isfinite(scale) || scale <= 0.0) {
    throw std::runtime_error(prefix + "metadata `" + std::string(UtNet2Spec::kMetadataOutputScale) +
                             "` is `" + scale_text + "`, expected a finite number above 0");
  }
  return static_cast<float>(scale);
}

}  // namespace alcedo::nn
