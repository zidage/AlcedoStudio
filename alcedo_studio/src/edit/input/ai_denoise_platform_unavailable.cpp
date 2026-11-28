//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The AI Denoise platform of a build without forward code (Linux before DN11, a macOS build
// that is not arm64): none, so a run reports the error (integration plan, D13).

#include "edit/input/ai_denoise_platform.hpp"

namespace alcedo {

auto CreateProductAiDenoisePlatform() -> std::unique_ptr<AiDenoisePlatform> { return nullptr; }

}  // namespace alcedo
