//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>
#include <memory>
#include <unordered_map>

#include "op_base.hpp"

namespace alcedo {

inline std::string OperatorTypeToString(OperatorType type) {
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
    default:
      return "UNKNOWN_OPERATOR";
  }
}

inline OperatorType OperatorTypeFromString(const std::string& type_str) {
  if (type_str == "RAW_DECODE")
    return OperatorType::RAW_DECODE;
  else if (type_str == "RESIZE")
    return OperatorType::RESIZE;
  else if (type_str == "CROP_ROTATE")
    return OperatorType::CROP_ROTATE;
  else if (type_str == "EXPOSURE")
    return OperatorType::EXPOSURE;
  else if (type_str == "CONTRAST")
    return OperatorType::CONTRAST;
  else if (type_str == "WHITE")
    return OperatorType::WHITE;
  else if (type_str == "BLACK")
    return OperatorType::BLACK;
  else if (type_str == "SHADOWS")
    return OperatorType::SHADOWS;
  else if (type_str == "HIGHLIGHTS")
    return OperatorType::HIGHLIGHTS;
  else if (type_str == "CURVE")
    return OperatorType::CURVE;
  else if (type_str == "HLS")
    return OperatorType::HLS;
  else if (type_str == "SATURATION")
    return OperatorType::SATURATION;
  else if (type_str == "TINT")
    return OperatorType::TINT;
  else if (type_str == "VIBRANCE")
    return OperatorType::VIBRANCE;
  else if (type_str == "CST")
    return OperatorType::CST;
  else if (type_str == "LMT")
    return OperatorType::LMT;
  else if (type_str == "ODT")
    return OperatorType::ODT;
  else if (type_str == "CLARITY")
    return OperatorType::CLARITY;
  else if (type_str == "SHARPEN")
    return OperatorType::SHARPEN;
  else if (type_str == "COLOR_WHEEL")
    return OperatorType::COLOR_WHEEL;
  else if (type_str == "ACES_TONE_MAPPING")
    return OperatorType::ACES_TONE_MAPPING;
  else if (type_str == "AUTO_EXPOSURE")
    return OperatorType::AUTO_EXPOSURE;
  else if (type_str == "LENS_CALIBRATION")
    return OperatorType::LENS_CALIBRATION;
  else if (type_str == "COLOR_TEMP")
    return OperatorType::COLOR_TEMP;
  else if (type_str == "FILM_GRAIN")
    return OperatorType::FILM_GRAIN;
  else if (type_str == "HALATION")
    return OperatorType::HALATION;
  else
    return static_cast<OperatorType>(-1);  // Unknown operator
}

class OperatorFactory {
 public:
  using Creator = std::function<std::shared_ptr<IOperatorBase>(const nlohmann::json&)>;

  static auto Instance() -> OperatorFactory&;

  void        Register(const OperatorType& type, Creator creator);

  auto        Create(const OperatorType& type, const nlohmann::json& params = {}) const
      -> std::shared_ptr<IOperatorBase>;

  template <typename T>
  static Creator MakeCreator() {
    return [](const nlohmann::json& params) -> std::shared_ptr<IOperatorBase> {
      auto op = std::make_shared<T>(params);
      return std::static_pointer_cast<IOperatorBase>(op);
    };
  }

 private:
  std::unordered_map<OperatorType, Creator> creators_;
};
}  // namespace alcedo
