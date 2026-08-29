//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>

namespace alcedo {

enum class TextureFormat : std::uint8_t {
  R8      = 0,
  Rgba8   = 1,
  R32f    = 2,
  Rgba32f = 3,
  R16u    = 4,
};

[[nodiscard]] inline auto TextureFormatName(TextureFormat format) -> const char* {
  switch (format) {
    case TextureFormat::R8:
      return "R8";
    case TextureFormat::Rgba8:
      return "Rgba8";
    case TextureFormat::R32f:
      return "R32f";
    case TextureFormat::Rgba32f:
      return "Rgba32f";
    case TextureFormat::R16u:
      return "R16u";
  }
  return "?";
}

[[nodiscard]] inline auto TextureFormatBytesPerPixel(TextureFormat format) -> std::size_t {
  switch (format) {
    case TextureFormat::R8:
      return 1;
    case TextureFormat::Rgba8:
      return 4;
    case TextureFormat::R32f:
      return 4;
    case TextureFormat::Rgba32f:
      return 16;
    case TextureFormat::R16u:
      return 2;
  }
  return 1;
}

}  // namespace alcedo
