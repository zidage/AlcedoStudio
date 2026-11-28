//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The AI Denoise platform of Windows x64: UtNet2DirectMlSession on the high-performance adapter
// (integration plan, D2, D13).

#include <memory>

#include "decoders/processor/nn/utnet2_directml_session.hpp"
#include "edit/input/ai_denoise_platform.hpp"

namespace alcedo {
namespace {

class DirectMlAiDenoisePlatform final : public AiDenoisePlatform {
 public:
  [[nodiscard]] auto DeviceKey() -> std::string override { return nn::DirectMlAdapterKey(); }

  void               CheckRuntime() override { nn::LoadDirectMlRuntime(); }

  [[nodiscard]] auto CreateSession(const std::filesystem::path& graph_path,
                                   std::string_view /*model_version*/)
      -> std::unique_ptr<nn::UtNet2TileSession> override {
    return std::make_unique<nn::UtNet2DirectMlSession>(graph_path);
  }
};

}  // namespace

auto CreateProductAiDenoisePlatform() -> std::unique_ptr<AiDenoisePlatform> {
  return std::make_unique<DirectMlAiDenoisePlatform>();
}

}  // namespace alcedo
