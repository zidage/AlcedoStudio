//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "sidecar_client/dto/runtime.hpp"
#include "sidecar_client/dto/semantic_embedding.hpp"

namespace alcedo::sidecar_client {

class SemanticEmbeddingClient {
 public:
  virtual ~SemanticEmbeddingClient() = default;

  virtual auto Ping(std::chrono::milliseconds timeout, std::string* error) -> bool = 0;
  virtual auto GetRuntimeStatus(std::chrono::milliseconds timeout,
                                AiSidecarRuntimeRemoteStatus* status, std::string* error)
      -> bool = 0;
  virtual auto GetModelInfo(std::chrono::milliseconds timeout, AiSidecarRuntimeModelInfo* info,
                            std::string* error) -> bool = 0;
  virtual auto EmbedText(const std::string& request_id, const std::string& text,
                         std::chrono::milliseconds timeout) -> SemanticEmbeddingResult = 0;
  virtual auto EmbedTextBatch(const std::vector<SemanticTextEmbeddingRequest>& requests,
                              std::chrono::milliseconds timeout)
      -> std::vector<SemanticEmbeddingResult> = 0;
  virtual auto EmbedImage(const std::string& request_id, const std::vector<uint8_t>& rgba8_image,
                          const std::string& format_hint, std::chrono::milliseconds timeout)
      -> SemanticEmbeddingResult = 0;
  virtual auto EmbedImageBatch(std::vector<SemanticImageEmbeddingRequest> requests,
                               std::chrono::milliseconds timeout)
      -> std::vector<SemanticEmbeddingResult> = 0;
};

}  // namespace alcedo::sidecar_client
