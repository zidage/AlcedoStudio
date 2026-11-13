//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <string>

#include "sidecar_client/model_manager_client.hpp"
#include "sidecar_client/semantic_embedding_client.hpp"

namespace alcedo::sidecar_client {

class Client {
 public:
  virtual ~Client() = default;

  virtual auto endpoint() const -> const std::string& = 0;
  virtual auto models() -> ModelManagerClient& = 0;
  virtual auto semantic() -> SemanticEmbeddingClient& = 0;
};

auto MakeGrpcClient(std::string endpoint) -> std::shared_ptr<Client>;

}  // namespace alcedo::sidecar_client
