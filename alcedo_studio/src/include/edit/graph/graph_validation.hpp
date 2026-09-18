//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>

#include "edit/graph/graph_ids.hpp"
#include "edit/mask/mask_id.hpp"
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
  DeletionProtected,
};

struct GraphValidationError {
  GraphValidationCode code = GraphValidationCode::Ok;
  std::string         message;
  NodeId              node_id;
  MaskId              mask_id;
};

}  // namespace alcedo
