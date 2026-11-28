//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <string_view>

namespace alcedo {

/**
 * @brief True when @p text is an AI Denoise model version: the SHA-256 of the FP16 graph in 64
 *        lowercase hexadecimal digits (AI Denoise integration plan, D10).
 */
[[nodiscard]] inline auto IsAiDenoiseModelVersion(std::string_view text) -> bool {
  return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

}  // namespace alcedo
