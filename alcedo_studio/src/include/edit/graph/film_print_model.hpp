//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace alcedo {

/// Stored slider ranges of the film print.
inline constexpr float                            kFilmLightSpreadMin   = 0.0f;
inline constexpr float                            kFilmLightSpreadMax   = 3.0f;
inline constexpr float                            kFilmHalationMin      = 0.0f;
inline constexpr float                            kFilmHalationMax      = 8.0f;
inline constexpr float                            kFilmEnlargerFlareMin = 0.0f;
inline constexpr float                            kFilmEnlargerFlareMax = 0.1f;

/// Film identifiers that a document can store. These are the lists of the stock bundle, and a lab
/// test compares them with the shipped files. The Model reads no resource file.
inline constexpr std::array<std::string_view, 30> kFilmNegativeIds      = {
    "agfa_vista_100",     "fuji_c200",
    "fuji_eterna_500",    "fuji_eterna_500_vivid",
    "fuji_natura_1600",   "fuji_pro_160c",
    "fuji_pro_160s",      "fuji_pro_400h",
    "fuji_superia_reala", "fuji_superia_xtra_400",
    "kodak_5203",         "kodak_5206",
    "kodak_5207",         "kodak_5213",
    "kodak_5219",         "kodak_5247",
    "kodak_5247_II",      "kodak_5248",
    "kodak_5250",         "kodak_5277",
    "kodak_5293",         "kodak_aerocolor",
    "kodak_ektar_100",    "kodak_exr_5248",
    "kodak_gold_200",     "kodak_portra_160",
    "kodak_portra_400",   "kodak_portra_800",
    "kodak_ultramax_400", "kodak_vericolor_iii"};

/// Print material identifiers. See @ref kFilmNegativeIds.
inline constexpr std::array<std::string_view, 18> kFilmPrintMaterialIds = {
    "fuji_3513di",         "fuji_3523XD",         "fuji_ca_dpII",         "fuji_ca_maxima",
    "fuji_ca_pdII",        "fuji_ca_super_c",     "fujiflex_new",         "fujiflex_old",
    "kodak_2383",          "kodak_2393",          "kodak_5381",           "kodak_5383",
    "kodak_5384",          "kodak_duraflex_plus", "kodak_endura_premier", "kodak_exr_5386",
    "kodak_portra_endura", "kodak_supra_endura"};

/// Scene light identifiers: the reference, 5 lights, and 5 scenes. See @ref kFilmNegativeIds.
inline constexpr std::array<std::string_view, 11> kFilmSceneLightIds = {
    "reference",  "daylight",      "tungsten",       "fluorescent",
    "led",        "sodium",        "street_mixed",   "street_sodium",
    "street_led", "interior_warm", "interior_office"};

/**
 * @brief Stored fields of the film print display transform.
 *
 * All sliders act on the image. Only the three selections choose host tables.
 */
struct FilmPrintParams {
  std::string negative       = "kodak_5207";
  std::string print_material = "kodak_2383";
  std::string scene_light    = "reference";
  float       light_spread   = 1.0f;
  float       halation       = 1.0f;
  float       enlarger_flare = 0.01f;
};

/// True when @p id is in the identifier list @p ids.
template <std::size_t Count>
[[nodiscard]] constexpr auto IsFilmIdentifier(const std::array<std::string_view, Count>& ids,
                                              std::string_view id) -> bool {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace alcedo
