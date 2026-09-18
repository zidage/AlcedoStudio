//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "edit/operators/models/i_operator_model.hpp"

namespace alcedo {

using OperatorModelFactory = std::function<std::unique_ptr<IOperatorModel>()>;

struct AdjustmentDefinition {
  OperatorTypeId       type;
  std::string_view     display_name;
  OperatorModelFactory create_default_model;
};

/**
 * @brief Built-in adjustment Model factories. Backend GPU factories are added in
 * later phases.
 *
 * Registration rejects duplicate type text and duplicate FNV-1a hashes.
 * Thread-safe after construction: the catalog is immutable following static init.
 */
class BuiltinAdjustmentCatalog {
 public:
  static auto Instance() -> const BuiltinAdjustmentCatalog&;

  [[nodiscard]] auto Find(const OperatorTypeId& type) const -> const AdjustmentDefinition*;
  [[nodiscard]] auto Find(std::string_view text) const -> const AdjustmentDefinition*;
  [[nodiscard]] auto Definitions() const -> const std::vector<AdjustmentDefinition>&;

  /**
   * @brief Create a default Model for @p type.
   * @return nullptr when the type is not registered.
   */
  [[nodiscard]] auto CreateDefault(const OperatorTypeId& type) const
      -> std::unique_ptr<IOperatorModel>;

 private:
  BuiltinAdjustmentCatalog();
  void Register(AdjustmentDefinition definition);

  std::vector<AdjustmentDefinition> definitions_;
};

}  // namespace alcedo
