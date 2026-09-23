//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/history/edit_transaction.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "edit/pipeline/pipeline.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {
static std::string TruncateForUi(std::string s, std::size_t max_chars) {
  if (max_chars == 0 || s.size() <= max_chars) {
    return s;
  }
  if (max_chars <= 3) {
    return s.substr(0, max_chars);
  }
  s.resize(max_chars - 3);
  s += "...";
  return s;
}

static std::string JsonScalarToString(const nlohmann::json& v) {
  if (v.is_string()) {
    return v.get<std::string>();
  }
  if (v.is_number_float()) {
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(3);
    oss << v.get<double>();
    return oss.str();
  }
  if (v.is_number_integer()) {
    return std::to_string(v.get<long long>());
  }
  if (v.is_number_unsigned()) {
    return std::to_string(v.get<unsigned long long>());
  }
  if (v.is_boolean()) {
    return v.get<bool>() ? "true" : "false";
  }
  if (v.is_null()) {
    return "null";
  }
  return v.dump();
}

static bool IsLutPathEmpty(const nlohmann::json& params) {
  if (!params.is_object() || !params.contains("ocio_lmt")) {
    return true;
  }
  try {
    const auto path = params["ocio_lmt"].get<std::string>();
    return path.empty();
  } catch (...) {
    return true;
  }
}

static auto ExtractEmbeddedEnabled(const nlohmann::json& params) -> std::optional<bool> {
  if (!params.is_object()) {
    return std::nullopt;
  }

  if (params.contains("enabled") && params["enabled"].is_boolean()) {
    return params["enabled"].get<bool>();
  }

  if (params.size() == 1) {
    const auto& value = params.begin().value();
    if (value.is_object() && value.contains("enabled") && value["enabled"].is_boolean()) {
      return value["enabled"].get<bool>();
    }
  }

  return std::nullopt;
}

static auto ResolveStageEnableForParams(OperatorType type, const nlohmann::json& params) -> bool {
  if (type == OperatorType::LMT) {
    return !IsLutPathEmpty(params);
  }
  if (const auto embedded_enabled = ExtractEmbeddedEnabled(params); embedded_enabled.has_value()) {
    return *embedded_enabled;
  }
  return true;
}

static auto ParamsArePresent(const nlohmann::json& params) -> bool {
  return !params.is_null();
}

static auto ApplyOperatorState(PipelineExecutor& pipeline, PipelineStageName stage_name,
                               OperatorType operator_type, const nlohmann::json& params,
                               bool enabled) -> bool {
  auto& stage         = pipeline.GetStage(stage_name);
  auto& global_params = pipeline.GetGlobalParams();
  if (ParamsArePresent(params)) {
    stage.SetOperator(operator_type, params, global_params);
  }
  stage.EnableOperator(operator_type, enabled, global_params);
  return true;
}
}  // namespace

void EditTransaction::SetCreateTime() {
  created_time_ = std::chrono::system_clock::to_time_t(TimeProvider::Now());
}

auto EditTransaction::TransactionTypeToString(TransactionType type) -> const char* {
  switch (type) {
    case TransactionType::_ADD:
      return "ADD";
    case TransactionType::_DELETE:
      return "DELETE";
    case TransactionType::_EDIT:
      return "EDIT";
  }
  return "UNKNOWN";
}

auto EditTransaction::OperatorTypeToString(OperatorType type) -> const char* {
  switch (type) {
    case OperatorType::RAW_DECODE:
      return "RAW_DECODE";
    case OperatorType::RESIZE:
      return "RESIZE";
    case OperatorType::CROP_ROTATE:
      return "CROP_ROTATE";
    case OperatorType::EXPOSURE:
      return "EXPOSURE";
    case OperatorType::CONTRAST:
      return "CONTRAST";
    case OperatorType::WHITE:
      return "WHITE";
    case OperatorType::BLACK:
      return "BLACK";
    case OperatorType::SHADOWS:
      return "SHADOWS";
    case OperatorType::HIGHLIGHTS:
      return "HIGHLIGHTS";
    case OperatorType::CURVE:
      return "CURVE";
    case OperatorType::HLS:
      return "HLS";
    case OperatorType::SATURATION:
      return "SATURATION";
    case OperatorType::TINT:
      return "TINT";
    case OperatorType::VIBRANCE:
      return "VIBRANCE";
    case OperatorType::CST:
      return "CST";
    case OperatorType::TO_WS:
      return "TO_WS";
    case OperatorType::TO_OUTPUT:
      return "TO_OUTPUT";
    case OperatorType::LMT:
      return "LMT";
    case OperatorType::ODT:
      return "ODT";
    case OperatorType::CLARITY:
      return "CLARITY";
    case OperatorType::SHARPEN:
      return "SHARPEN";
    case OperatorType::COLOR_WHEEL:
      return "COLOR_WHEEL";
    case OperatorType::ACES_TONE_MAPPING:
      return "ACES_TONE_MAPPING";
    case OperatorType::AUTO_EXPOSURE:
      return "AUTO_EXPOSURE";
    case OperatorType::LENS_CALIBRATION:
      return "LENS_CALIBRATION";
    case OperatorType::COLOR_TEMP:
      return "COLOR_TEMP";
    case OperatorType::FILM_GRAIN:
      return "FILM_GRAIN";
    case OperatorType::HALATION:
      return "HALATION";
    case OperatorType::UNKNOWN:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

auto EditTransaction::StageNameToString(PipelineStageName stage) -> const char* {
  switch (stage) {
    case PipelineStageName::Image_Loading:
      return "Image_Loading";
    case PipelineStageName::Geometry_Adjustment:
      return "Geometry_Adjustment";
    case PipelineStageName::To_WorkingSpace:
      return "To_WorkingSpace";
    case PipelineStageName::Basic_Adjustment:
      return "Basic_Adjustment";
    case PipelineStageName::Color_Adjustment:
      return "Color_Adjustment";
    case PipelineStageName::Detail_Adjustment:
      return "Detail_Adjustment";
    case PipelineStageName::Output_Transform:
      return "Output_Transform";
    case PipelineStageName::Stage_Count:
      return "Stage_Count";
    case PipelineStageName::Merged_Stage:
      return "Merged_Stage";
  }
  return "UnknownStage";
}

auto EditTransaction::Describe(bool include_params, std::size_t max_params_chars) const
    -> std::string {
  std::ostringstream oss;
  oss << "#" << tx_id_ << " " << TransactionTypeToString(type_) << " "
      << StageNameToString(stage_name_)
      << "/" << OperatorTypeToString(operator_type_);
  if (!include_params) {
    return oss.str();
  }

  if (!after_params_.is_object()) {
    oss << " " << TruncateForUi(after_params_.dump(), max_params_chars);
    return oss.str();
  }

  std::vector<std::string> parts;
  parts.reserve(after_params_.size());

  for (auto it = after_params_.begin(); it != after_params_.end(); ++it) {
    const std::string& key = it.key();
    const auto&        val = it.value();

    if (before_params_.is_object() && before_params_.contains(key)) {
      const auto& old_val = before_params_[key];
      if (old_val == val) {
        continue;
      }
      parts.push_back(key + ": " + JsonScalarToString(old_val) + " -> " + JsonScalarToString(val));
    } else {
      parts.push_back(key + ": " + JsonScalarToString(val));
    }
  }

  if (parts.empty()) {
    oss << " " << TruncateForUi(after_params_.dump(), max_params_chars);
    return oss.str();
  }

  // Keep the summary compact for UI lists.
  constexpr std::size_t kMaxKeys = 3;
  if (parts.size() > kMaxKeys) {
    parts.resize(kMaxKeys);
    parts.push_back("...");
  }

  std::string joined;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) {
      joined += ", ";
    }
    joined += parts[i];
  }

  oss << " " << TruncateForUi(joined, max_params_chars);
  return oss.str();
}

auto EditTransaction::ApplyForward(PipelineExecutor& pipeline) const -> bool {
  const bool enabled = after_enabled_ &&
                       (!ParamsArePresent(after_params_) ||
                        ResolveStageEnableForParams(operator_type_, after_params_));
  return ApplyOperatorState(pipeline, stage_name_, operator_type_, after_params_, enabled);
}

auto EditTransaction::ApplyBackward(PipelineExecutor& pipeline) const -> bool {
  const bool enabled = before_enabled_ &&
                       (!ParamsArePresent(before_params_) ||
                        ResolveStageEnableForParams(operator_type_, before_params_));
  return ApplyOperatorState(pipeline, stage_name_, operator_type_, before_params_, enabled);
}

auto EditTransaction::ToJSON() const -> nlohmann::json {
  nlohmann::json j;
  j["id"]             = tx_id_;
  j["type"]           = static_cast<int>(type_);
  j["operator_type"]  = static_cast<int>(operator_type_);
  j["stage_name"]     = static_cast<int>(stage_name_);
  j["before_params"]  = before_params_;
  j["after_params"]   = after_params_;
  j["before_enabled"] = before_enabled_;
  j["after_enabled"]  = after_enabled_;
  j["created_time"]      = created_time_;
  j["transaction_hash"]  = GetTransactionHash().ToString();

  return j;
}

void EditTransaction::FromJSON(const nlohmann::json& j) {
  tx_id_          = j["id"].get<tx_id_t>();
  type_           = static_cast<TransactionType>(j["type"].get<int>());
  operator_type_  = static_cast<OperatorType>(j["operator_type"].get<int>());
  stage_name_     = static_cast<PipelineStageName>(j["stage_name"].get<int>());
  before_params_  = j.value("before_params", nlohmann::json::object());
  after_params_   = j.contains("after_params") ? j["after_params"] : j["operator_params"];
  before_enabled_ = j.value("before_enabled", j.contains("last_operator_params"));
  after_enabled_  = j.value("after_enabled", true);
  created_time_   = j.value("created_time", std::time_t{0});
  if (created_time_ == 0) {
    SetCreateTime();
  }
  if (j.contains("last_operator_params")) {
    before_params_ = j["last_operator_params"];
  }
  if (j.contains("transaction_hash") && j.at("transaction_hash").is_string()) {
    transaction_hash_ = Hash128::FromString(j.at("transaction_hash").get<std::string>());
  } else {
    GenerateTransactionHash();
  }
}

};  // namespace alcedo
