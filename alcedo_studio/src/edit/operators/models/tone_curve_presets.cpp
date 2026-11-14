//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/models/tone_curve_presets.hpp"

#include <algorithm>
#include <array>

namespace alcedo {
namespace {

inline constexpr std::array<CurvePoint, 2> kLinearPoints{{{0.0f, 0.0f}, {1.0f, 1.0f}}};

#include "tone_curve_preset_points.inc"

const std::array<ToneCurvePreset, 9> kPresets{{
    {"linear", "Linear", "", kLinearPoints},
    {"soft_contrast", "Soft Contrast", "Contrast", kSoftContrastPoints},
    {"medium_contrast", "Medium Contrast", "Contrast", kMediumContrastPoints},
    {"strong_contrast", "Strong Contrast", "Contrast", kStrongContrastPoints},
    {kFilmLikeToneCurvePresetId, "Film-like", "Rendering", kFilmLikePoints},
    {"filmic", "Filmic", "Rendering", kFilmicPoints},
    {"kodak_2383", "Kodak 2383", "Film", kKodak2383Points},
    {"fuji_velvia_50", "Velvia 50", "Film", kFujiVelvia50Points},
    {"kodak_ektachrome_e100", "Ektachrome E100", "Film", kKodakEktachromeE100Points},
}};

}  // namespace

auto ToneCurvePreset::PointList() const -> std::vector<CurvePoint> {
  return {points.begin(), points.end()};
}

auto ToneCurvePresets() -> std::span<const ToneCurvePreset> { return kPresets; }

auto FindToneCurvePreset(std::string_view id) -> const ToneCurvePreset* {
  const auto found = std::ranges::find(kPresets, id, &ToneCurvePreset::id);
  return found == kPresets.end() ? nullptr : &*found;
}

auto MatchToneCurvePreset(std::span<const CurvePoint> points) -> const ToneCurvePreset* {
  const auto found = std::ranges::find_if(kPresets, [points](const ToneCurvePreset& preset) {
    return std::ranges::equal(preset.points, points);
  });
  return found == kPresets.end() ? nullptr : &*found;
}

}  // namespace alcedo
