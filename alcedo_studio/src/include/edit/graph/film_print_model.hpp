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
/// test compares them with the shipped files. The Model reads no resource file. The order is the
/// order of the editor lists (lab decision U83): the common stocks first, then Fuji, then the old
/// Kodak stocks. A document stores the identifier, so the order has no effect on a document.
inline constexpr std::array<std::string_view, 30> kFilmNegativeIds = {
    "kodak_5207",
    "kodak_5219",
    "kodak_5213",
    "kodak_5203",
    "kodak_5206",
    "kodak_portra_400",
    "kodak_portra_160",
    "kodak_portra_800",
    "kodak_ektar_100",
    "kodak_gold_200",
    "kodak_ultramax_400",
    "fuji_eterna_500",
    "fuji_eterna_500_vivid",
    "fuji_pro_400h",
    "fuji_pro_160c",
    "fuji_pro_160s",
    "fuji_natura_1600",
    "fuji_superia_xtra_400",
    "fuji_superia_reala",
    "fuji_c200",
    "kodak_5277",
    "kodak_5293",
    "kodak_exr_5248",
    "kodak_5248",
    "kodak_5247",
    "kodak_5247_II",
    "kodak_5250",
    "kodak_vericolor_iii",
    "kodak_aerocolor",
    "agfa_vista_100"};

/// Print material `negative_scan` (lab step P14): no print. It selects the Negative Scan form of
/// FP6: a virtual film scanner reads the negative, a straight-line inversion with one gamma per
/// medium about the grey of the balance light of the stock makes the positive, and the OpenDRT of
/// the document shows it. The stock bundle names it in its metadata `negative_scan`, not in its list
/// of print materials.
inline constexpr std::string_view kFilmNegativeScanId = "negative_scan";

/// Print material identifiers. The first is @ref kFilmNegativeScanId; the others are the print
/// materials of the stock bundle. See @ref kFilmNegativeIds.
inline constexpr std::array<std::string_view, 20> kFilmPrintMaterialIds = {
    kFilmNegativeScanId,
    "cine_look",
    "kodak_2383",
    "kodak_endura_premier",
    "kodak_portra_endura",
    "kodak_supra_endura",
    "kodak_2393",
    "fuji_3513di",
    "fuji_3523XD",
    "fuji_ca_maxima",
    "fuji_ca_dpII",
    "fuji_ca_pdII",
    "fuji_ca_super_c",
    "fujiflex_new",
    "fujiflex_old",
    "kodak_duraflex_plus",
    "kodak_5383",
    "kodak_5384",
    "kodak_5381",
    "kodak_exr_5386"};

/// Catalog names of @ref kFilmNegativeIds, in the same order. A lab test compares them with the
/// shipped files.
inline constexpr std::array<std::string_view, 30> kFilmNegativeNames = {
    "Kodak Vision3 250D 5207",
    "Kodak Vision3 500T 5219",
    "Kodak Vision3 200T 5213",
    "Kodak Vision3 50D 5203",
    "Kodak Verita 200D 5206",
    "Kodak Portra 400",
    "Kodak Portra 160",
    "Kodak Portra 800",
    "Kodak Ektar 100",
    "Kodak Gold 200",
    "Kodak Ultramax 400",
    "Fuji Eterna 500",
    "Fuji Eterna 500 Vivid",
    "Fuji Pro 400H",
    "Fuji Pro 160C",
    "Fuji Pro 160S",
    "Fuji Natura 1600",
    "Fuji Superia X-Tra 400",
    "Fuji Superia Reala",
    "Fuji C200",
    "Kodak Vision 320T 5277",
    "Kodak EXR 200T 5293",
    "Kodak EXR 100T 5248",
    "Kodak 5248",
    "Kodak 5247",
    "Kodak 5247 II",
    "Kodak 5250",
    "Kodak Vericolor III",
    "Kodak Aerocolor IV 2460",
    "Agfa Vista 100"};

/// Catalog names of @ref kFilmPrintMaterialIds, in the same order.
inline constexpr std::array<std::string_view, 20> kFilmPrintMaterialNames = {
    "Negative Scan",
    "Cine Look (2383)",
    "Kodak Vision 2383",
    "Kodak Endura Premier Paper",
    "Kodak Portra Endura Paper",
    "Kodak Supra Endura Paper",
    "Kodak Vision Premier 2393",
    "Fuji Eterna-CP Type 3513DI",
    "Fuji Eterna-CP 3523XD",
    "Fuji Crystal Archive Maxima",
    "Fuji Crystal Archive DPII",
    "Fuji Crystal Archive Pro PDII",
    "Fuji Crystal Archive Super Type C",
    "Fujiflex Crystal Archive New Version",
    "Fujiflex Crystal Archive Old Version",
    "Kodak Duraflex Plus",
    "Kodak 5383",
    "Kodak 5384",
    "Kodak 5381",
    "Kodak EXR 5386"};

/// Colour reversal films (slide films) on a light table, in the order of the lab list (lab decision
/// U86). A document stores one of them in the field of @ref kFilmNegativeIds, and it selects the
/// light table form of the method, which has no print material. See @ref kFilmNegativeIds.
inline constexpr std::array<std::string_view, 5> kFilmReversalIds = {
    "fuji_velvia_50", "fuji_velvia_100", "fuji_provia_100f", "fuji_astia_100f",
    "kodak_ektachrome_e100"};

/// Catalog names of @ref kFilmReversalIds, in the same order.
inline constexpr std::array<std::string_view, 5> kFilmReversalNames = {
    "Fujichrome Velvia 50", "Fujichrome Velvia 100", "Fujichrome Provia 100F",
    "Fujichrome Astia 100F", "Kodak Ektachrome E100"};

/// Scene light `as_shot` (lab decision U92): the light of the colour temperature of the RAW decode.
/// The stock bundle holds no light of this name: the host blends two lights of its colour
/// temperature grid. An input with no RAW white balance reads it as `reference`. It is the default
/// scene light of every form (issue 672).
inline constexpr std::string_view                 kFilmAsShotLightId = "as_shot";

/// Scene light identifiers: the reference, As Shot, 5 lights, and 5 scenes. See
/// @ref kFilmNegativeIds. The stock bundle has each of them except @ref kFilmAsShotLightId.
inline constexpr std::array<std::string_view, 12> kFilmSceneLightIds = {
    "reference", "as_shot",      "daylight",      "tungsten",   "fluorescent",   "led",
    "sodium",    "street_mixed", "street_sodium", "street_led", "interior_warm", "interior_office"};

/// Rule `frame_auto_colour` of Negative Scan: `off` is Neutral Scan, with no correction from the
/// frame. `warm` and `cool` keep a small part of the cast along the light axis.
inline constexpr std::array<std::string_view, 4> kFilmScanAutoIds = {"off", "neutral", "warm",
                                                                     "cool"};
/// The light of the virtual scanner of Negative Scan: a Planckian radiator at 3200 K, or an
/// equal-energy light.
inline constexpr std::array<std::string_view, 2> kFilmScannerLightIds = {"halogen",
                                                                         "equal_energy"};

/// Stored ranges of the Negative Scan settings: the ranges of the group "Negative scan" of the lab
/// viewer (`mvp_pipeline.CONTROLS`).
inline constexpr float kFilmScanAutoStrengthMin   = 0.0f;
inline constexpr float kFilmScanAutoStrengthMax   = 1.0f;
inline constexpr float kFilmScanAutoDensityMin    = 0.0f;
inline constexpr float kFilmScanAutoDensityMax    = 1.0f;
inline constexpr float kFilmScanContrastMin       = 0.5f;
inline constexpr float kFilmScanContrastMax       = 1.5f;
inline constexpr float kFilmScanExposureStopsMin  = -3.0f;
inline constexpr float kFilmScanExposureStopsMax  = 3.0f;
inline constexpr float kFilmScanKeyMin            = -0.3f;  ///< Cyan, magenta, yellow (log10)
inline constexpr float kFilmScanKeyMax            = 0.3f;
inline constexpr float kFilmScanBlackMin          = 0.0f;
inline constexpr float kFilmScanBlackMax          = 1.5f;
inline constexpr float kFilmScannerRedMinNm       = 600.0f;
inline constexpr float kFilmScannerRedMaxNm       = 720.0f;
inline constexpr float kFilmScannerGreenMinNm     = 500.0f;
inline constexpr float kFilmScannerGreenMaxNm     = 590.0f;
inline constexpr float kFilmScannerBlueMinNm      = 400.0f;
inline constexpr float kFilmScannerBlueMaxNm      = 490.0f;
inline constexpr float kFilmScannerWidthMinNm     = 10.0f;
inline constexpr float kFilmScannerWidthMaxNm     = 120.0f;

/**
 * @brief Stored settings of Negative Scan (lab step P14, issue 661).
 *
 * The rendering settings act after the scan, so a change of one of them keeps the scanner table.
 * The scanner settings (the bands and the light) choose the scanner table. The defaults are those
 * of the lab viewer: Neutral Scan (auto colour off), contrast 1, no key, black at the film base,
 * and the default scanner (650, 545, and 450 nm, 40 nm wide, halogen). A key at its default is not
 * written to JSON.
 */
struct FilmNegativeScanParams {
  std::string auto_colour      = "off";  ///< JSON key `auto`, one of @ref kFilmScanAutoIds
  float       auto_strength    = 0.8f;
  float       auto_density     = 0.0f;
  float       contrast         = 1.0f;  ///< JSON key `scan_contrast`
  float       exposure_stops   = 0.0f;  ///< JSON key `scan_exposure_stops`
  float       cyan             = 0.0f;  ///< log10 units of the positive; takes red away
  float       magenta          = 0.0f;
  float       yellow           = 0.0f;
  float       black            = 1.0f;  ///< Share of the film base that is the black
  float       scanner_red_nm   = 650.0f;
  float       scanner_green_nm = 545.0f;
  float       scanner_blue_nm  = 450.0f;
  float       scanner_width_nm = 40.0f;  ///< Full width at half maximum of each band
  std::string scanner_light    = "halogen";  ///< One of @ref kFilmScannerLightIds

  auto operator==(const FilmNegativeScanParams&) const -> bool = default;
};

/**
 * @brief Stored fields of the film print display transform.
 *
 * All sliders act on the image. Only the three selections and the scanner settings choose host
 * tables. A colour reversal film in @ref negative has no print, so it reads no @ref print_material,
 * and @ref enlarger_flare is the flare of the loupe on the light table. Print material
 * @ref kFilmNegativeScanId has no enlarger, so it reads no @ref enlarger_flare, and the stored
 * value stays. Only Negative Scan reads @ref scan.
 */
struct FilmPrintParams {
  std::string            negative       = "kodak_5207";
  std::string            print_material = "kodak_endura_premier";
  std::string            scene_light    = std::string{kFilmAsShotLightId};
  float                  light_spread   = 1.0f;
  float                  halation       = 1.0f;
  float                  enlarger_flare = 0.01f;
  FilmNegativeScanParams scan{};
};

/// True when @p id is in the identifier list @p ids.
template <std::size_t Count>
[[nodiscard]] constexpr auto IsFilmIdentifier(const std::array<std::string_view, Count>& ids,
                                              std::string_view id) -> bool {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

/// True when @p id is a colour reversal film, which has the light table form and no print.
[[nodiscard]] constexpr auto IsFilmReversalIdentifier(std::string_view id) -> bool {
  return IsFilmIdentifier(kFilmReversalIds, id);
}

/// True when @p id is a value of the field `negative`: a negative or a colour reversal film.
[[nodiscard]] constexpr auto IsFilmStockIdentifier(std::string_view id) -> bool {
  return IsFilmIdentifier(kFilmNegativeIds, id) || IsFilmReversalIdentifier(id);
}

/// True when @p id is @ref kFilmAsShotLightId.
[[nodiscard]] constexpr auto IsFilmAsShotLight(std::string_view id) -> bool {
  return id == kFilmAsShotLightId;
}

/**
 * True when @p negative on @p print_material has the Negative Scan form of FP6: print material
 * @ref kFilmNegativeScanId with a negative. A colour reversal film reads no print material, so a
 * stored `negative_scan` has no effect on it.
 */
[[nodiscard]] constexpr auto IsFilmNegativeScan(std::string_view negative,
                                                std::string_view print_material) -> bool {
  return print_material == kFilmNegativeScanId && !IsFilmReversalIdentifier(negative);
}

}  // namespace alcedo
