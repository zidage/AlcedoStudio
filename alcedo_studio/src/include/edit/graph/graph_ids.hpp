//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace alcedo {

/**
 * @brief Stable identifier for a node in a PipelineGraph.
 *
 * Default document uses "develop", "grade.primary", and "drt".
 */
class NodeId {
 public:
  NodeId() = default;
  explicit NodeId(std::string value) : value_(std::move(value)) {}

  [[nodiscard]] auto Value() const -> std::string_view { return value_; }
  [[nodiscard]] auto Empty() const -> bool { return value_.empty(); }

  friend auto operator==(const NodeId& lhs, const NodeId& rhs) -> bool {
    return lhs.value_ == rhs.value_;
  }
  friend auto operator!=(const NodeId& lhs, const NodeId& rhs) -> bool { return !(lhs == rhs); }
  friend auto operator<(const NodeId& lhs, const NodeId& rhs) -> bool {
    return lhs.value_ < rhs.value_;
  }

 private:
  std::string value_;
};

/**
 * @brief Named port on a node ("image", "mask", "display").
 */
class PortId {
 public:
  PortId() = default;
  explicit PortId(std::string value) : value_(std::move(value)) {}

  [[nodiscard]] auto Value() const -> std::string_view { return value_; }
  [[nodiscard]] auto Empty() const -> bool { return value_.empty(); }

  friend auto operator==(const PortId& lhs, const PortId& rhs) -> bool {
    return lhs.value_ == rhs.value_;
  }
  friend auto operator!=(const PortId& lhs, const PortId& rhs) -> bool { return !(lhs == rhs); }

 private:
  std::string value_;
};

/**
 * @brief Identity of one adjustment instance inside a ColorGrade node.
 *
 * Distinct from OperatorTypeId so the same adjustment type can appear twice.
 */
class AdjustmentInstanceId {
 public:
  AdjustmentInstanceId() = default;
  explicit AdjustmentInstanceId(std::string value) : value_(std::move(value)) {}

  [[nodiscard]] auto Value() const -> std::string_view { return value_; }
  [[nodiscard]] auto Empty() const -> bool { return value_.empty(); }

  friend auto operator==(const AdjustmentInstanceId& lhs, const AdjustmentInstanceId& rhs)
      -> bool {
    return lhs.value_ == rhs.value_;
  }
  friend auto operator!=(const AdjustmentInstanceId& lhs, const AdjustmentInstanceId& rhs)
      -> bool {
    return !(lhs == rhs);
  }

 private:
  std::string value_;
};

/**
 * @brief Identity of a produced graph value: producer node plus output port.
 *
 * Workspace KV cache (later phases) keys results by this id.
 */
struct GraphValueId {
  NodeId producer;
  PortId output_port;

  friend auto operator==(const GraphValueId& lhs, const GraphValueId& rhs) -> bool {
    return lhs.producer == rhs.producer && lhs.output_port == rhs.output_port;
  }
};

}  // namespace alcedo
