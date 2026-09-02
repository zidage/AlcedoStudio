//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>

namespace alcedo {

enum class GraphValidationCode {
  Ok = 0,
  DuplicateNodeId,
  MissingDevelopEndpoint,
  MissingDrtEndpoint,
  MultipleDevelopEndpoints,
  MultipleDrtEndpoints,
  UnknownNode,
  UnknownPort,
  PortTypeMismatch,
  Cycle,
  MissingRequiredInput,
  MultipleInputsOnPort,
  SceneImageFanOut,
  ColorGradeNotOnImageBackbone,
  BrokenImageBackbone,
  ProtectedEndpoint,
  NotAColorGrade,
  InvalidDisplayName,
  InvalidNodeValue,
};

struct GraphValidationError {
  GraphValidationCode code = GraphValidationCode::Ok;
  std::string         message;
};

}  // namespace alcedo
