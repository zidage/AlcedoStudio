//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "edit/graph/film_print_model.hpp"

namespace alcedo {

/// List of the film print browser that shows a film or a print material.
enum class FilmBrowserList {
  /// The negatives (the field `negative`).
  Negative,
  /// The colour reversal films (also the field `negative`).
  Reversal,
  /// The print materials (the field `print_material`).
  Print,
};

/// Swatch shape of a film or a print material. The shape shows the material.
enum class FilmSwatchShape {
  /// A tab in two colours: the brand colour, then the stock colour.
  PhotoNegative,
  /// A dark film can with perforations and one stock colour.
  CinemaNegative,
  /// A slide mount. The window has one band for each colour, from the top.
  Reversal,
  /// A white sheet with an edge from the first colour to the second colour.
  PrintPaper,
  /// A clear strip with perforations, from the first colour to the second colour.
  CinemaPrint,
  /// A glossy sheet, from the first colour to the second colour.
  DisplayMaterial,
  /// A negative frame in the first colour on a scanner bed, with a light bar in the second colour.
  Scan,
};

/// A brand of the film print browser, with the colour of its heading and filter chip.
struct FilmBrand {
  std::string_view name;
  std::uint32_t    color;  ///< 0xRRGGBB
};

/// How the film print browser shows one film or print material.
struct FilmPresentation {
  std::string_view             id;
  FilmBrowserList              list;
  /// Brand name of @ref kFilmBrands. Empty for an entry above the brand groups (Lab Scan, Cine
  /// Look).
  std::string_view             brand;
  /// English family caption under the brand (the panel translates it). Can be empty.
  std::string_view             family;
  /// Name without the brand. Product names are not translated.
  std::string_view             short_label;
  FilmSwatchShape              shape;
  std::array<std::uint32_t, 3> colors;  ///< 0xRRGGBB, the first @ref color_count are used.
  std::size_t                  color_count;
};

/// The brands of the browser, in browser order.
inline constexpr std::array<FilmBrand, 3> kFilmBrands = {{
    {"Kodak", 0xF2B705},
    {"Fujifilm", 0x1F9E5A},
    {"Agfa", 0xD7262E},
}};

namespace film_presentation_detail {
inline constexpr std::uint32_t kKodak      = 0xF2B705;
inline constexpr std::uint32_t kFuji       = 0x1F9E5A;
inline constexpr std::uint32_t kAgfa       = 0xD7262E;
/// One colour for the Portra films (user decision 2026-11-03).
inline constexpr std::uint32_t kPortra     = 0x6B4FA0;
/// Box colours of the colour reversal films: the identification colours of the Fujifilm product
/// bulletins (Velvia 50: gold and blue; Velvia 100 and Astia 100F: violet blue) on the Fujichrome
/// green, and the box photos of Provia 100F and Ektachrome E100.
inline constexpr std::uint32_t kFujichrome = 0x169E4F;
inline constexpr std::uint32_t kVioletBlue = 0x3A3FA0;

constexpr auto Neg(std::string_view id, std::string_view brand, std::string_view family,
                   std::string_view short_label, std::uint32_t ground, std::uint32_t accent)
    -> FilmPresentation {
  return {id,
          FilmBrowserList::Negative,
          brand,
          family,
          short_label,
          FilmSwatchShape::PhotoNegative,
          {ground, accent, 0},
          2};
}

constexpr auto Cine(std::string_view id, std::string_view brand, std::string_view family,
                    std::string_view short_label, std::uint32_t accent) -> FilmPresentation {
  return {id,          FilmBrowserList::Negative,       brand,          family,
          short_label, FilmSwatchShape::CinemaNegative, {accent, 0, 0}, 1};
}

constexpr auto Slide(std::string_view id, std::string_view brand, std::string_view short_label,
                     std::array<std::uint32_t, 3> colors, std::size_t count) -> FilmPresentation {
  return {id,          FilmBrowserList::Reversal, brand,  "",
          short_label, FilmSwatchShape::Reversal, colors, count};
}

constexpr auto Print(std::string_view id, std::string_view brand, std::string_view family,
                     std::string_view short_label, FilmSwatchShape shape, std::uint32_t from,
                     std::uint32_t to) -> FilmPresentation {
  return {id, FilmBrowserList::Print, brand, family, short_label, shape, {from, to, 0}, 2};
}
}  // namespace film_presentation_detail

/**
 * @brief The films and print materials of the film print browser, in browser order.
 *
 * Each list is grouped by brand in the order of @ref kFilmBrands, then by family. The colours are
 * an abstract form of the box design, except the reversal films, which use the colours of the box.
 * A document stores only the identifier, so this table has no effect on a document.
 */
inline constexpr auto kFilmPresentations = [] {
  using namespace film_presentation_detail;
  using S = FilmSwatchShape;
  return std::array<FilmPresentation, kFilmNegativeIds.size() + kFilmReversalIds.size() +
                                          kFilmPrintMaterialIds.size()>{{
      // Negatives.
      Neg("kodak_portra_400", "Kodak", "Photo", "Portra 400", kKodak, kPortra),
      Neg("kodak_portra_160", "Kodak", "Photo", "Portra 160", kKodak, kPortra),
      Neg("kodak_portra_800", "Kodak", "Photo", "Portra 800", kKodak, kPortra),
      Neg("kodak_ektar_100", "Kodak", "Photo", "Ektar 100", kKodak, 0xE04A1F),
      Neg("kodak_gold_200", "Kodak", "Photo", "Gold 200", 0xE8A400, 0xC8232C),
      Neg("kodak_ultramax_400", "Kodak", "Photo", "Ultramax 400", kKodak, 0x2446A8),
      Neg("kodak_vericolor_iii", "Kodak", "Photo", "Vericolor III", 0xC99A3B, 0x5A3B22),
      Cine("kodak_5207", "Kodak", "Cinema", "Vision3 250D 5207", 0xF08A24),
      Cine("kodak_5219", "Kodak", "Cinema", "Vision3 500T 5219", 0x3B5BDB),
      Cine("kodak_5213", "Kodak", "Cinema", "Vision3 200T 5213", 0x5C86E0),
      Cine("kodak_5203", "Kodak", "Cinema", "Vision3 50D 5203", 0xF2D06B),
      Cine("kodak_5206", "Kodak", "Cinema", "Verita 200D 5206", 0xF2725A),
      Cine("kodak_5277", "Kodak", "Classic Cinema", "Vision 320T 5277", 0x7D88C4),
      Cine("kodak_5293", "Kodak", "Classic Cinema", "EXR 200T 5293", 0x8494BD),
      Cine("kodak_exr_5248", "Kodak", "Classic Cinema", "EXR 100T 5248", 0x93A2BB),
      Cine("kodak_5248", "Kodak", "Classic Cinema", "5248", 0x9EA9BA),
      Cine("kodak_5247", "Kodak", "Classic Cinema", "5247", 0xA7AAB4),
      Cine("kodak_5247_II", "Kodak", "Classic Cinema", "5247 II", 0xB4B6BD),
      Cine("kodak_5250", "Kodak", "Classic Cinema", "5250", 0xBCAE8C),
      Neg("kodak_aerocolor", "Kodak", "Aerial", "Aerocolor IV 2460", kKodak, 0x7FB4D8),
      Neg("fuji_pro_400h", "Fujifilm", "Photo", "Pro 400H", kFuji, 0x8EC9E8),
      Neg("fuji_pro_160c", "Fujifilm", "Photo", "Pro 160C", kFuji, 0xE86E5A),
      Neg("fuji_pro_160s", "Fujifilm", "Photo", "Pro 160S", kFuji, 0xD9CBA0),
      Neg("fuji_natura_1600", "Fujifilm", "Photo", "Natura 1600", 0x0F5E3A, 0xD8B04A),
      Neg("fuji_superia_xtra_400", "Fujifilm", "Photo", "Superia X-Tra 400", kFuji, 0xD33A2C),
      Neg("fuji_superia_reala", "Fujifilm", "Photo", "Superia Reala", kFuji, 0x2BB3C0),
      Neg("fuji_c200", "Fujifilm", "Photo", "C200", kFuji, 0xF1E36A),
      Cine("fuji_eterna_500", "Fujifilm", "Cinema", "Eterna 500", 0x2FA36B),
      Cine("fuji_eterna_500_vivid", "Fujifilm", "Cinema", "Eterna 500 Vivid", 0x22C7A0),
      Neg("agfa_vista_100", "Agfa", "Photo", "Vista 100", kAgfa, 0xF2F2F2),
      // Colour reversal films.
      Slide("fuji_velvia_50", "Fujifilm", "Velvia 50", {kFujichrome, 0xC9A86A, 0x3F5BC8}, 3),
      Slide("fuji_velvia_100", "Fujifilm", "Velvia 100", {kFujichrome, kVioletBlue, 0xF2F2F2}, 3),
      Slide("fuji_provia_100f", "Fujifilm", "Provia 100F", {kFujichrome, 0x4B509A, 0}, 2),
      Slide("fuji_astia_100f", "Fujifilm", "Astia 100F", {kFujichrome, kVioletBlue, 0}, 2),
      Slide("kodak_ektachrome_e100", "Kodak", "Ektachrome E100", {0x0B2FB8, 0xE6AD58, 0}, 2),
      // Print materials. Lab Scan and Cine Look are above the brand groups. Lab Scan shows the
      // orange mask of a colour negative under the light of a scanner.
      Print("lab_scan", "", "", "Lab Scan", S::Scan, 0xC8702C, 0xEAF4FF),
      Print("cine_look", "", "", "Cine Look (2383)", S::CinemaPrint, kKodak, 0xE07A1F),
      Print("kodak_2383", "Kodak", "Cinema Print", "Vision 2383", S::CinemaPrint, kKodak, 0xE07A1F),
      Print("kodak_2393", "Kodak", "Cinema Print", "Vision Premier 2393", S::CinemaPrint, kKodak,
            0xC8232C),
      Print("kodak_5383", "Kodak", "Cinema Print", "5383", S::CinemaPrint, kKodak, 0xD9A441),
      Print("kodak_5384", "Kodak", "Cinema Print", "5384", S::CinemaPrint, kKodak, 0xC9A86A),
      Print("kodak_5381", "Kodak", "Cinema Print", "5381", S::CinemaPrint, kKodak, 0xB8A27E),
      Print("kodak_exr_5386", "Kodak", "Cinema Print", "EXR 5386", S::CinemaPrint, kKodak,
            0xA89F8C),
      Print("kodak_endura_premier", "Kodak", "Paper", "Endura Premier", S::PrintPaper, kKodak,
            0xC98A2A),
      Print("kodak_portra_endura", "Kodak", "Paper", "Portra Endura", S::PrintPaper, kKodak,
            kPortra),
      Print("kodak_supra_endura", "Kodak", "Paper", "Supra Endura", S::PrintPaper, kKodak,
            0x3E7FB0),
      Print("kodak_duraflex_plus", "Kodak", "Display", "Duraflex Plus", S::DisplayMaterial, kKodak,
            0xC0C6CC),
      Print("fuji_3513di", "Fujifilm", "Cinema Print", "Eterna-CP 3513DI", S::CinemaPrint, kFuji,
            0x2FA36B),
      Print("fuji_3523XD", "Fujifilm", "Cinema Print", "Eterna-CP 3523XD", S::CinemaPrint, kFuji,
            0x22C7A0),
      Print("fuji_ca_maxima", "Fujifilm", "Paper", "Crystal Archive Maxima", S::PrintPaper, kFuji,
            0xD8B04A),
      Print("fuji_ca_dpII", "Fujifilm", "Paper", "Crystal Archive DPII", S::PrintPaper, kFuji,
            0x8EC9E8),
      Print("fuji_ca_pdII", "Fujifilm", "Paper", "Crystal Archive Pro PDII", S::PrintPaper, kFuji,
            0x6B4FA0),
      Print("fuji_ca_super_c", "Fujifilm", "Paper", "Crystal Archive Super Type C", S::PrintPaper,
            kFuji, 0xE86E5A),
      Print("fujiflex_new", "Fujifilm", "Display", "Fujiflex New Version", S::DisplayMaterial,
            kFuji, 0xC0C6CC),
      Print("fujiflex_old", "Fujifilm", "Display", "Fujiflex Old Version", S::DisplayMaterial,
            kFuji, 0x9AA3A8),
  }};
}();

/// Default film of each film list: the film that a change of process selects first.
inline constexpr std::string_view kFilmDefaultNegative = "kodak_5207";
inline constexpr std::string_view kFilmDefaultReversal = "fuji_provia_100f";

/// The browser entry of @p id, or none when @p id is not a film or a print material.
[[nodiscard]] constexpr auto      FindFilmPresentation(std::string_view id)
    -> std::optional<FilmPresentation> {
  for (const auto& entry : kFilmPresentations) {
    if (entry.id == id) {
      return entry;
    }
  }
  return std::nullopt;
}

}  // namespace alcedo
