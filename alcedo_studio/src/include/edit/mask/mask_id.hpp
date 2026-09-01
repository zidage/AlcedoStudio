//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace alcedo {

/**
 * @brief Stable identity for one Color Grade Mask during its lifetime.
 *
 * Distinct from @ref NodeId and from display-list index. Empty values are invalid.
 */
class MaskId {
 public:
  MaskId() = default;
  explicit MaskId(std::string value) : value_(std::move(value)) {}

  [[nodiscard]] auto Value() const -> std::string_view { return value_; }
  [[nodiscard]] auto Empty() const -> bool { return value_.empty(); }

  friend auto operator==(const MaskId& lhs, const MaskId& rhs) -> bool {
    return lhs.value_ == rhs.value_;
  }
  friend auto operator!=(const MaskId& lhs, const MaskId& rhs) -> bool { return !(lhs == rhs); }
  friend auto operator<(const MaskId& lhs, const MaskId& rhs) -> bool {
    return lhs.value_ < rhs.value_;
  }

 private:
  std::string value_;
};

}  // namespace alcedo
