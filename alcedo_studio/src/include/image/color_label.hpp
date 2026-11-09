//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>

namespace alcedo {

/**
 * @brief Color label codes of a library photo (#768).
 *
 * The value is the bit index in `Image.color_labels`: bit k set means the photo has code k. The
 * database stores the codes only; the name of each code is a setting of the computer. Stored
 * values never change meaning.
 */
enum class ColorLabel : std::uint8_t {
  kRed    = 0,
  kOrange = 1,
  kYellow = 2,
  kGreen  = 3,
  kBlue   = 4,
  kPurple = 5,
  kGray   = 6,
};

/// Bit set of color label codes (`Image.color_labels`, DuckDB `UTINYINT`).
using ColorLabelMask = std::uint8_t;

/// Number of color label codes.
inline constexpr int            kColorLabelCount   = 7;
/// Every valid code bit. Bit 7 is reserved.
inline constexpr ColorLabelMask kColorLabelMaskAll = 0x7F;

/// The bit of @p code.
[[nodiscard]] constexpr auto ColorLabelBit(ColorLabel code) -> ColorLabelMask {
  return static_cast<ColorLabelMask>(1u << static_cast<unsigned>(code));
}

/// True when @p mask holds @p code.
[[nodiscard]] constexpr auto HasColorLabel(ColorLabelMask mask, ColorLabel code) -> bool {
  return (mask & ColorLabelBit(code)) != 0;
}

/// @p mask with @p code set (@p on) or cleared; the other codes stay.
[[nodiscard]] constexpr auto WithColorLabel(ColorLabelMask mask, ColorLabel code, bool on)
    -> ColorLabelMask {
  return on ? static_cast<ColorLabelMask>(mask | ColorLabelBit(code))
            : static_cast<ColorLabelMask>(mask & ~ColorLabelBit(code));
}

/// The code of index @p index (0 to kColorLabelCount - 1), or empty for any other value.
[[nodiscard]] constexpr auto ColorLabelFromIndex(int index) -> std::optional<ColorLabel> {
  if (index < 0 || index >= kColorLabelCount) {
    return std::nullopt;
  }
  return static_cast<ColorLabel>(index);
}

}  // namespace alcedo
