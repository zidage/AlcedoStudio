//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/operator_factory.hpp"

#include <memory>

namespace alcedo {
auto OperatorFactory::Instance() -> OperatorFactory& {
  static OperatorFactory instance;
  return instance;
}

void OperatorFactory::Register(const OperatorType& type, Creator creator) {
  creators_[type] = creator;
}

auto OperatorFactory::Create(const OperatorType& type, const nlohmann::json& params) const
    -> std::shared_ptr<IOperatorBase> {
  auto it = creators_.find(type);
  return (it != creators_.end()) ? (it->second)(params) : nullptr;
}
};  // namespace alcedo