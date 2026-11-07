//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace alcedo::sidecar_client {

class CredentialClient {
 public:
  virtual ~CredentialClient() = default;

  virtual auto RegisterCredential(const std::string& provider_id, const std::string& secret,
                                  int64_t ttl_ms, std::chrono::milliseconds timeout,
                                  std::string* handle, std::string* error) -> bool = 0;
  virtual auto RevokeCredential(const std::string& handle, std::chrono::milliseconds timeout,
                                bool* revoked, std::string* error) -> bool = 0;
};

}  // namespace alcedo::sidecar_client
