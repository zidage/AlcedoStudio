//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// DuckDB value handles shared by the duckorm sources (Statement binds and the Appender). Not
// part of the duckorm API.

#pragma once

#include <duckdb.h>

#include <span>

namespace duckorm::internal {

/// Owns one `duckdb_value` and destroys it.
class DuckValueGuard {
 public:
  explicit DuckValueGuard(duckdb_value value = nullptr) : value_(value) {}
  DuckValueGuard(const DuckValueGuard&)                    = delete;
  auto operator=(const DuckValueGuard&) -> DuckValueGuard& = delete;
  DuckValueGuard(DuckValueGuard&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
  auto operator=(DuckValueGuard&& other) noexcept -> DuckValueGuard& {
    if (this != &other) {
      Reset();
      value_       = other.value_;
      other.value_ = nullptr;
    }
    return *this;
  }
  ~DuckValueGuard() { Reset(); }

  [[nodiscard]] auto     get() const -> duckdb_value { return value_; }
  [[nodiscard]] explicit operator bool() const { return value_ != nullptr; }

 private:
  void Reset() {
    if (value_ != nullptr) {
      duckdb_destroy_value(&value_);
      value_ = nullptr;
    }
  }

  duckdb_value value_ = nullptr;
};

/// A `FLOAT[n]` array value with n = `values.size()`. Throws std::runtime_error on failure.
[[nodiscard]] auto MakeFloatArrayValue(std::span<const float> values) -> DuckValueGuard;

}  // namespace duckorm::internal
