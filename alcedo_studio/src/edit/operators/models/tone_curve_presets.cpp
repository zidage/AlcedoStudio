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

// Earlier fits of Film-like (tone_curve_preset_study.md, section 7). Kept by hand: the fit script
// writes only the current points.
// clang-format off
// #891, before the highlight shoulder of #899.
inline constexpr std::array<CurvePoint, 12> kFilmLikePointsBeforeShoulder{{{0.00000f, 0.14493f}, {0.24811f, 0.26545f}, {0.28192f, 0.28731f}, {0.32285f, 0.32092f}, {0.36954f, 0.36613f}, {0.41359f, 0.41359f}, {0.50517f, 0.53603f}, {0.58131f, 0.66565f}, {0.62018f, 0.77121f}, {0.64790f, 0.92653f}, {0.94031f, 0.94154f}, {1.00000f, 0.95434f}}};
// #899, before the shadow toe of #910.
inline constexpr std::array<CurvePoint, 13> kFilmLikePointsBeforeToe{{{0.00000f, 0.14493f}, {0.24811f, 0.26545f}, {0.28192f, 0.28731f}, {0.32285f, 0.32092f}, {0.36954f, 0.36613f}, {0.41359f, 0.41359f}, {0.50517f, 0.53603f}, {0.57179f, 0.64589f}, {0.59180f, 0.68831f}, {0.63221f, 0.76256f}, {0.66469f, 0.79612f}, {0.71636f, 0.84267f}, {1.00000f, 0.95434f}}};
// clang-format on

const std::array<std::span<const CurvePoint>, 2> kFilmLikeEarlierPoints{
    kFilmLikePointsBeforeShoulder, kFilmLikePointsBeforeToe};

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

auto FilmLikeToneCurveEarlierPoints() -> std::span<const std::span<const CurvePoint>> {
  return kFilmLikeEarlierPoints;
}

auto IsFilmLikeToneCurve(std::span<const CurvePoint> points) -> bool {
  return std::ranges::equal(kFilmLikePoints, points) ||
         std::ranges::any_of(kFilmLikeEarlierPoints, [points](std::span<const CurvePoint> earlier) {
           return std::ranges::equal(earlier, points);
         });
}

}  // namespace alcedo
