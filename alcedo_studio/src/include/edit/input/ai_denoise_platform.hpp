//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "decoders/processor/nn/utnet2_tile_loop.hpp"

namespace alcedo {

/**
 * @brief The platform part of AI Denoise for @ref DenoiseModelSession (integration plan, DN6):
 *        the device that a run uses and the creation of a platform session on it.
 *
 * Windows: UtNet2DirectMlSession on the high-performance adapter. macOS (DN10): the Core ML
 * session. Tests give their own platform. One call at a time (the owner serializes them).
 */
class AiDenoisePlatform {
 public:
  virtual ~AiDenoisePlatform()                          = default;

  /**
   * @brief The identity of the device that a new platform session uses (Windows: the
   *        description and the LUID of the adapter). Cheap: creates no session.
   * @throws std::runtime_error when the platform has no device for the model.
   */
  [[nodiscard]] virtual auto DeviceKey() -> std::string = 0;

  /**
   * @brief Load the FP16 graph at @p graph_path into a new platform session.
   * @throws std::runtime_error with the error of the runtime (ONNX Runtime, DirectML, Direct3D
   *         12) or of the graph check.
   */
  [[nodiscard]] virtual auto CreateSession(const std::filesystem::path& graph_path)
      -> std::unique_ptr<nn::UtNet2TileSession> = 0;
};

/**
 * @brief The platform of this build: DirectML on Windows x64 with ONNX Runtime; null on a
 *        platform whose forward code does not exist yet (D13).
 */
[[nodiscard]] auto CreateProductAiDenoisePlatform() -> std::unique_ptr<AiDenoisePlatform>;

}  // namespace alcedo
