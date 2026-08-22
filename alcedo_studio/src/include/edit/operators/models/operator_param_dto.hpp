//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#include "edit/graph/graph_ids.hpp"
#include "edit/operators/models/dirty_field_mask.hpp"
#include "edit/operators/models/operator_type_id.hpp"

namespace alcedo {

/**
 * @brief Type-erased immutable parameter payload. Contains no GPU or JSON types.
 */
class IOperatorParamPayload {
 public:
  virtual ~IOperatorParamPayload() = default;

  [[nodiscard]] virtual auto Type() const -> OperatorTypeId        = 0;
  [[nodiscard]] virtual auto DataVersion() const -> std::uint32_t = 0;
};

/**
 * @brief Concrete payload box for one Model's parameter struct.
 */
template <class T>
class TypedOperatorParamPayload final : public IOperatorParamPayload {
 public:
  TypedOperatorParamPayload(OperatorTypeId type, std::uint32_t version, T value)
      : type_(std::move(type)), version_(version), value_(std::move(value)) {}

  [[nodiscard]] auto Type() const -> OperatorTypeId override { return type_; }
  [[nodiscard]] auto DataVersion() const -> std::uint32_t override { return version_; }
  [[nodiscard]] auto Value() const -> const T& { return value_; }

 private:
  OperatorTypeId type_;
  std::uint32_t  version_ = 1;
  T              value_{};
};

/**
 * @brief Full parameter snapshot used to build ParameterArena and recover devices.
 *
 * Independent of dirty state. payload is shared and immutable.
 */
struct OperatorParamDto {
  OperatorTypeId                               type;
  std::uint32_t                                data_version = 1;
  std::shared_ptr<const IOperatorParamPayload> payload;
};

/**
 * @brief Dirty-field patch taken from a Model. node_id and adjustment_id may be
 * empty when taken directly from an IOperatorModel; ColorGrade fills them.
 */
struct OperatorParamPatchDto {
  NodeId                                       node_id;
  AdjustmentInstanceId                         adjustment_id;
  OperatorTypeId                               type;
  DirtyFieldMask                               dirty_fields;
  std::shared_ptr<const IOperatorParamPayload> payload;
};

template <class T>
[[nodiscard]] auto PayloadAs(const IOperatorParamPayload* payload) -> const T* {
  const auto* typed = dynamic_cast<const TypedOperatorParamPayload<T>*>(payload);
  return typed == nullptr ? nullptr : &typed->Value();
}

}  // namespace alcedo
