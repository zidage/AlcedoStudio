//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "sidecar_client/dto/runtime.hpp"

namespace alcedo::sidecar_client {

class RuntimeControlClient {
 public:
  virtual ~RuntimeControlClient() = default;

  virtual auto Ping(std::chrono::milliseconds timeout, std::string* error) -> bool = 0;
  virtual auto GetRuntimeStatus(std::chrono::milliseconds timeout,
                                AiSidecarRuntimeRemoteStatus* status, std::string* error)
      -> bool = 0;
  virtual auto ListCapabilities(std::chrono::milliseconds timeout,
                                std::vector<AiSidecarCapability>* capabilities,
                                std::string* error) -> bool = 0;
  virtual auto CancelTask(const std::string& request_id, std::chrono::milliseconds timeout,
                          bool* cancelled, std::string* error) -> bool = 0;
};

}  // namespace alcedo::sidecar_client
