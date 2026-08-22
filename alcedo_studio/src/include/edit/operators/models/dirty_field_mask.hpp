//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <type_traits>

namespace alcedo {

/**
 * @brief Bit mask of dirty parameter fields. A field is clean or dirty; there is
 * no write counter.
 *
 * Combine with bitwise or. Empty mask means no dirty fields.
 */
class DirtyFieldMask {
 public:
  constexpr DirtyFieldMask() = default;

  constexpr explicit DirtyFieldMask(std::uint64_t bits) : bits_(bits) {}

  template <typename Enum, typename = std::enable_if_t<std::is_enum_v<Enum>>>
  constexpr DirtyFieldMask(Enum bit) : bits_(static_cast<std::uint64_t>(bit)) {}

  [[nodiscard]] constexpr auto Bits() const -> std::uint64_t { return bits_; }
  [[nodiscard]] constexpr auto Any() const -> bool { return bits_ != 0; }
  [[nodiscard]] constexpr auto Contains(DirtyFieldMask other) const -> bool {
    return (bits_ & other.bits_) == other.bits_ && other.bits_ != 0;
  }

  constexpr auto operator|=(DirtyFieldMask other) -> DirtyFieldMask& {
    bits_ |= other.bits_;
    return *this;
  }

  friend constexpr auto operator|(DirtyFieldMask lhs, DirtyFieldMask rhs) -> DirtyFieldMask {
    return DirtyFieldMask{lhs.bits_ | rhs.bits_};
  }

  friend constexpr auto operator&(DirtyFieldMask lhs, DirtyFieldMask rhs) -> DirtyFieldMask {
    return DirtyFieldMask{lhs.bits_ & rhs.bits_};
  }

  friend constexpr auto operator==(DirtyFieldMask lhs, DirtyFieldMask rhs) -> bool {
    return lhs.bits_ == rhs.bits_;
  }

  friend constexpr auto operator!=(DirtyFieldMask lhs, DirtyFieldMask rhs) -> bool {
    return lhs.bits_ != rhs.bits_;
  }

 private:
  std::uint64_t bits_ = 0;
};

}  // namespace alcedo
