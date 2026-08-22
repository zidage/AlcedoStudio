//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <span>

#include "edit/graph/graph_ids.hpp"
#include "edit/graph/port.hpp"
#include "edit/operators/models/operator_type_id.hpp"
#include "json.hpp"

namespace alcedo {

/**
 * @brief User-assemblable graph node. Does not execute GPU work.
 */
class INodeModel {
 public:
  virtual ~INodeModel() = default;

  [[nodiscard]] virtual auto Id() const -> const NodeId&                 = 0;
  [[nodiscard]] virtual auto Type() const -> const OperatorTypeId&       = 0;
  [[nodiscard]] virtual auto InputPorts() const -> std::span<const PortDescriptor>  = 0;
  [[nodiscard]] virtual auto OutputPorts() const -> std::span<const PortDescriptor> = 0;

  /**
   * @brief Serialize node identity, type, and parameters. Does not include edges.
   */
  [[nodiscard]] virtual auto ToJson() const -> nlohmann::json = 0;
};

}  // namespace alcedo
