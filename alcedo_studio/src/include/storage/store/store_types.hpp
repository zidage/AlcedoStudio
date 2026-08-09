//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once
#include <duckdb.h>

#include <memory>
#include <mutex>

namespace alcedo {
class ConnectionGuard {
 public:
  duckdb_connection                     conn_;
  std::shared_ptr<std::recursive_mutex> db_lock_;

  ConnectionGuard(duckdb_connection conn, std::shared_ptr<std::recursive_mutex> db_lock = {});
  ConnectionGuard(ConnectionGuard&& other) noexcept;
  ~ConnectionGuard();

  [[nodiscard]] auto Lock() const -> std::unique_lock<std::recursive_mutex>;
};
}  // namespace alcedo
