//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/adjustment_ownership.hpp"

#include <stdexcept>
#include <string>

#include "edit/operators/models/builtin_type_ids.hpp"

namespace alcedo {
namespace {

auto OwnerName(AdjustmentParameterOwner owner) -> const char* {
  switch (owner) {
    case AdjustmentParameterOwner::ColorGrade:
      return "Color Grade";
    case AdjustmentParameterOwner::DrtPost:
      return "DRT/Post";
    case AdjustmentParameterOwner::Unsupported:
      return "an unsupported owner";
  }
  return "an unsupported owner";
}

}  // namespace

auto ColorGradeAdjustmentTypes() -> std::array<OperatorTypeId, 13> {
  return {type_ids::Cat02WhiteBalance(), type_ids::Exposure(), type_ids::Contrast(),
          type_ids::White(),             type_ids::Black(),    type_ids::Shadows(),
          type_ids::Highlights(),        type_ids::Curve(),    type_ids::Hls(),
          type_ids::Saturation(),        type_ids::Vibrance(), type_ids::ColorWheel(),
          type_ids::Lmt()};
}

auto DrtPostAdjustmentTypes() -> std::array<OperatorTypeId, 4> {
  return {type_ids::Clarity(), type_ids::Sharpen(), type_ids::Halation(), type_ids::FilmGrain()};
}

auto OwnerOfAdjustment(const OperatorTypeId& type) -> AdjustmentParameterOwner {
  for (const auto& owned : ColorGradeAdjustmentTypes()) {
    if (type == owned) {
      return AdjustmentParameterOwner::ColorGrade;
    }
  }
  for (const auto& owned : DrtPostAdjustmentTypes()) {
    if (type == owned) {
      return AdjustmentParameterOwner::DrtPost;
    }
  }
  return AdjustmentParameterOwner::Unsupported;
}

auto AdjustmentInstanceSuffix(const OperatorTypeId& type) -> std::string {
  if (type == type_ids::Cat02WhiteBalance()) {
    return "cat02_wb";
  }
  if (type == type_ids::Hls()) {
    return "hls";
  }
  const std::string text{type.Text()};
  const auto        pos = text.rfind('.');
  if (pos == std::string::npos || pos + 1 >= text.size()) {
    return text;
  }
  return text.substr(pos + 1);
}

auto MakeAdjustmentInstanceId(const NodeId& node_id, const OperatorTypeId& type)
    -> AdjustmentInstanceId {
  return AdjustmentInstanceId{std::string{node_id.Value()} + "." + AdjustmentInstanceSuffix(type)};
}

void RequireAdjustmentOwner(const OperatorTypeId& type, AdjustmentParameterOwner expected,
                            std::string_view context) {
  const auto actual = OwnerOfAdjustment(type);
  if (actual == expected) {
    return;
  }
  throw std::runtime_error(std::string{context} + ": " + std::string{type.Text()} +
                           " belongs to " + OwnerName(actual) + ", not " + OwnerName(expected));
}

void RequireCompleteDrtPostTypes(std::span<const OperatorTypeId> present,
                                 std::string_view                context) {
  const auto required = DrtPostAdjustmentTypes();
  if (present.size() != required.size()) {
    throw std::runtime_error(std::string{context} +
                             ": DRT/Post must contain Clarity, Sharpen, Halation, and Film Grain");
  }
  bool seen[4] = {};
  for (const auto& type : present) {
    RequireAdjustmentOwner(type, AdjustmentParameterOwner::DrtPost, context);
    bool matched = false;
    for (std::size_t index = 0; index < required.size(); ++index) {
      if (type != required[index]) {
        continue;
      }
      if (seen[index]) {
        throw std::runtime_error(std::string{context} + ": duplicate DRT/Post adjustment " +
                                 std::string{type.Text()});
      }
      seen[index] = true;
      matched     = true;
      break;
    }
    if (!matched) {
      throw std::runtime_error(std::string{context} + ": unexpected DRT/Post adjustment " +
                               std::string{type.Text()});
    }
  }
  for (std::size_t index = 0; index < required.size(); ++index) {
    if (!seen[index]) {
      throw std::runtime_error(std::string{context} + ": missing DRT/Post adjustment " +
                               std::string{required[index].Text()});
    }
  }
}

}  // namespace alcedo
