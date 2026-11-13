//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace alcedo {

/// FNV-1a 64-bit hash used for stable OperatorTypeId lookup.
/// @param text UTF-8 type string. Must be non-empty for registered types.
/// @return Deterministic hash. Collisions are rejected at catalog registration.
[[nodiscard]] constexpr auto Fnv1a64(std::string_view text) -> std::uint64_t {
  constexpr std::uint64_t kOffset = 14695981039346656037ull;
  constexpr std::uint64_t kPrime  = 1099511628211ull;
  std::uint64_t           hash    = kOffset;
  for (unsigned char byte : text) {
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= kPrime;
  }
  return hash;
}

/**
 * @brief Stable string type identifier for adjustments and nodes.
 *
 * JSON stores @ref Text. Runtime lookup may use @ref Hash; the catalog rejects
 * duplicate text and duplicate hashes at registration.
 *
 * Thread-safe: immutable after construction.
 */
class OperatorTypeId {
 public:
  OperatorTypeId() = default;

  explicit OperatorTypeId(std::string text)
      : text_(std::move(text)), hash_(Fnv1a64(text_)) {}

  [[nodiscard]] auto Text() const -> std::string_view { return text_; }
  [[nodiscard]] auto Hash() const -> std::uint64_t { return hash_; }
  [[nodiscard]] auto Empty() const -> bool { return text_.empty(); }

  friend auto operator==(const OperatorTypeId& lhs, const OperatorTypeId& rhs) -> bool {
    return lhs.hash_ == rhs.hash_ && lhs.text_ == rhs.text_;
  }

  friend auto operator!=(const OperatorTypeId& lhs, const OperatorTypeId& rhs) -> bool {
    return !(lhs == rhs);
  }

 private:
  std::string   text_;
  std::uint64_t hash_ = 0;
};

}  // namespace alcedo
