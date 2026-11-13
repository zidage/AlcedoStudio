//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/brush_stroke.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace alcedo {
namespace {

[[noreturn]] void FailStroke(std::string_view message) {
  throw std::runtime_error(std::string{message});
}

void RequireFiniteNumber(const nlohmann::json& json, const char* key, std::string_view owner) {
  if (!json.contains(key) || !json[key].is_number()) {
    FailStroke(std::string{owner} + " is missing number " + key);
  }
}

auto ReadFiniteFloat(const nlohmann::json& json, const char* key, std::string_view owner) -> float {
  RequireFiniteNumber(json, key, owner);
  const auto value = json[key].get<float>();
  if (!std::isfinite(value)) {
    FailStroke(std::string{key} + " must be finite");
  }
  return value;
}

auto SampleToJson(const BrushCanonicalSample& sample) -> nlohmann::json {
  return {{"local_x", sample.local_x},
          {"local_y", sample.local_y},
          {"radius", sample.radius},
          {"strength", sample.strength},
          {"hardness", sample.hardness}};
}

auto SampleFromJson(const nlohmann::json& json) -> BrushCanonicalSample {
  if (!json.is_object()) {
    FailStroke("stroke sample must be an object");
  }
  BrushCanonicalSample sample;
  sample.local_x  = ReadFiniteFloat(json, "local_x", "stroke sample");
  sample.local_y  = ReadFiniteFloat(json, "local_y", "stroke sample");
  sample.radius   = ReadFiniteFloat(json, "radius", "stroke sample");
  sample.strength = ReadFiniteFloat(json, "strength", "stroke sample");
  sample.hardness = ReadFiniteFloat(json, "hardness", "stroke sample");
  return sample;
}

void ValidateSampleEncoding(const BrushCanonicalSample& sample) {
  try {
    ValidateBrushCanonicalSample(sample);
  } catch (const std::invalid_argument& ex) {
    FailStroke(ex.what());
  }
}

}  // namespace

auto operator==(const BrushStroke& lhs, const BrushStroke& rhs) -> bool {
  if (lhs.id != rhs.id || lhs.mode != rhs.mode) {
    return false;
  }
  const auto lhs_empty = lhs.samples == nullptr || lhs.samples->empty();
  const auto rhs_empty = rhs.samples == nullptr || rhs.samples->empty();
  if (lhs_empty && rhs_empty) {
    return true;
  }
  if (lhs_empty || rhs_empty) {
    return false;
  }
  return *lhs.samples == *rhs.samples;
}

void ValidateBrushStroke(const BrushStroke& stroke) {
  if (stroke.id.Empty()) {
    FailStroke("StrokeId must not be empty");
  }
  if (stroke.mode != BrushStrokeMode::Paint && stroke.mode != BrushStrokeMode::Erase) {
    FailStroke("stroke mode must be paint or erase");
  }
  if (stroke.samples == nullptr || stroke.samples->empty()) {
    FailStroke("stroke samples must not be empty");
  }
  for (const auto& sample : *stroke.samples) {
    ValidateSampleEncoding(sample);
  }
}

void ValidateBrushStrokeList(std::span<const BrushStroke> strokes) {
  std::unordered_set<std::string> seen;
  seen.reserve(strokes.size());
  for (const auto& stroke : strokes) {
    ValidateBrushStroke(stroke);
    if (!seen.insert(std::string{stroke.id.Value()}).second) {
      FailStroke("Duplicate StrokeId: " + std::string{stroke.id.Value()});
    }
  }
}

auto MakeBrushStroke(StrokeId id, BrushStrokeMode mode, std::vector<BrushCanonicalSample> samples)
    -> BrushStroke {
  BrushStroke stroke;
  stroke.id   = std::move(id);
  stroke.mode = mode;
  if (samples.empty()) {
    FailStroke("stroke samples must not be empty");
  }
  for (const auto& sample : samples) {
    ValidateSampleEncoding(sample);
  }
  stroke.samples = std::make_shared<const std::vector<BrushCanonicalSample>>(std::move(samples));
  ValidateBrushStroke(stroke);
  return stroke;
}

auto FindBrushStrokeIndex(std::span<const BrushStroke> strokes, const StrokeId& stroke_id)
    -> std::size_t {
  for (std::size_t index = 0; index < strokes.size(); ++index) {
    if (strokes[index].id == stroke_id) {
      return index;
    }
  }
  return strokes.size();
}

auto BrushStrokeToJson(const BrushStroke& stroke) -> nlohmann::json {
  ValidateBrushStroke(stroke);
  nlohmann::json samples = nlohmann::json::array();
  for (const auto& sample : *stroke.samples) {
    samples.push_back(SampleToJson(sample));
  }
  return {{"id", std::string{stroke.id.Value()}},
          {"mode", static_cast<int>(stroke.mode)},
          {"samples", std::move(samples)}};
}

auto BrushStrokeFromJson(const nlohmann::json& json) -> BrushStroke {
  if (!json.is_object()) {
    FailStroke("stroke must be an object");
  }
  if (!json.contains("id") || !json["id"].is_string() || json["id"].get<std::string>().empty()) {
    FailStroke("stroke is missing a non-empty id");
  }
  if (!json.contains("mode") || !json["mode"].is_number() || json["mode"].is_number_float()) {
    FailStroke("stroke is missing integer mode");
  }
  const auto mode_value = json["mode"].get<std::int64_t>();
  if (mode_value != 0 && mode_value != 1) {
    FailStroke("stroke mode must be 0 (paint) or 1 (erase)");
  }
  if (!json.contains("samples") || !json["samples"].is_array() || json["samples"].empty()) {
    FailStroke("stroke is missing a non-empty samples array");
  }
  std::vector<BrushCanonicalSample> samples;
  samples.reserve(json["samples"].size());
  for (const auto& item : json["samples"]) {
    samples.push_back(SampleFromJson(item));
  }
  return MakeBrushStroke(StrokeId{json["id"].get<std::string>()},
                         static_cast<BrushStrokeMode>(mode_value), std::move(samples));
}

auto BrushStrokeListToJson(std::span<const BrushStroke> strokes) -> nlohmann::json {
  ValidateBrushStrokeList(strokes);
  nlohmann::json json = nlohmann::json::array();
  for (const auto& stroke : strokes) {
    json.push_back(BrushStrokeToJson(stroke));
  }
  return json;
}

auto BrushStrokeListFromJson(const nlohmann::json& json) -> std::vector<BrushStroke> {
  if (!json.is_array()) {
    FailStroke("strokes must be an array");
  }
  std::vector<BrushStroke> strokes;
  strokes.reserve(json.size());
  for (const auto& item : json) {
    strokes.push_back(BrushStrokeFromJson(item));
  }
  ValidateBrushStrokeList(strokes);
  return strokes;
}

}  // namespace alcedo
