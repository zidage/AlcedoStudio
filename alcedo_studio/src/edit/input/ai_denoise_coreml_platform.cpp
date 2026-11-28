//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The AI Denoise platform of macOS on Apple silicon: UtNet2CoreMlSession on the Neural Engine
// with the model cache of the installed model version (integration plan, D2, D13, DN10).

#include <memory>

#include "decoders/processor/nn/utnet2_coreml_session.hpp"
#include "edit/input/ai_denoise_platform.hpp"

namespace alcedo {
namespace {

class CoreMlAiDenoisePlatform final : public AiDenoisePlatform {
 public:
  [[nodiscard]] auto DeviceKey() -> std::string override { return nn::CoreMlDeviceKey(); }

  void               CheckRuntime() override { nn::LoadCoreMlRuntime(); }

  [[nodiscard]] auto CreateSession(const std::filesystem::path& graph_path,
                                   std::string_view             model_version)
      -> std::unique_ptr<nn::UtNet2TileSession> override {
    return std::make_unique<nn::UtNet2CoreMlSession>(
        graph_path, nn::PrepareCoreMlModelCache(nn::CoreMlModelCacheRoot(), model_version));
  }
};

}  // namespace

auto CreateProductAiDenoisePlatform() -> std::unique_ptr<AiDenoisePlatform> {
  return std::make_unique<CoreMlAiDenoisePlatform>();
}

}  // namespace alcedo
