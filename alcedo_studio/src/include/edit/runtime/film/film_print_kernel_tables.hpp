//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "edit/runtime/film/film_stock_bundle.hpp"

namespace alcedo::film_print_kernel_tables {

/// Layout of the table buffer of a selection. See `FilmKernelParams` in film_print_math.h.
struct Layout {
  std::size_t        uv_values         = 0;
  std::size_t        curve_values      = 0;
  std::size_t        inhibition_values = 0;
  std::size_t        facets            = 0;

  [[nodiscard]] auto CurvesOffset() const -> std::size_t { return uv_values; }
  [[nodiscard]] auto InhibitionOffset() const -> std::size_t { return uv_values + curve_values; }
  [[nodiscard]] auto NormalsOffset() const -> std::size_t {
    return InhibitionOffset() + inhibition_values;
  }
  [[nodiscard]] auto RoomOffset() const -> std::size_t { return NormalsOffset() + facets * 2; }
  [[nodiscard]] auto ValueCount() const -> std::size_t { return RoomOffset() + facets; }
};

[[nodiscard]] inline auto LayoutOf(const FilmStockBundle& bundle, std::string_view negative)
    -> Layout {
  const auto stock = bundle.Film(negative);
  const auto nodes = static_cast<std::size_t>(bundle.UvNodes());
  return {nodes * nodes * 3, stock.curves.density.size(), stock.inhibition.free_values.size(),
          bundle.PointConstants().locus_room.size()};
}

/**
 * @brief The table buffer of a selection, in the layout of @ref Layout.
 * @param uv_exposure The u'v' table of the selection, (uv_nodes, uv_nodes, 3).
 * @throws std::runtime_error when @p uv_exposure has another size.
 */
template <class Real>
[[nodiscard]] auto MakeValues(const FilmStockBundle& bundle, std::string_view negative,
                              std::span<const double> uv_exposure) -> std::vector<Real> {
  const auto layout = LayoutOf(bundle, negative);
  if (uv_exposure.size() != layout.uv_values) {
    throw std::runtime_error("Film print: the u'v' table of '" + std::string(negative) +
                             "' has an unexpected size");
  }
  const auto        stock     = bundle.Film(negative);
  const auto        constants = bundle.PointConstants();
  std::vector<Real> values(layout.ValueCount());
  const auto        copy = [&values](std::span<const double> from, std::size_t offset) {
    std::transform(from.begin(), from.end(), values.begin() + static_cast<std::ptrdiff_t>(offset),
                          [](double value) { return static_cast<Real>(value); });
  };
  copy(uv_exposure, 0);
  copy(stock.curves.density, layout.CurvesOffset());
  copy(stock.inhibition.free_values, layout.InhibitionOffset());
  copy(constants.locus_normals, layout.NormalsOffset());
  copy(constants.locus_room, layout.RoomOffset());
  return values;
}

/// The scalar values of a selection that FillParams reads: a print, or a light table.
struct SelectionValues {
  std::array<double, 3> layer_gain{};
  std::array<double, 9> xyz_to_display{};
  std::array<double, 3> negative_grade_grey{};
  bool                  cinema_look    = false;
  bool                  negative_grade = false;
  bool                  light_table    = false;
};

[[nodiscard]] inline auto SelectionValuesOf(const FilmStockBundle& bundle,
                                            std::string_view       negative,
                                            std::string_view       print_material,
                                            std::string_view       scene_light) -> SelectionValues {
  if (bundle.IsReversal(negative)) {
    // A colour reversal film has no print, so the print material is not read.
    const auto setup = bundle.ReversalSelection(negative, scene_light);
    return {.layer_gain     = setup.layer_gain,
            .xyz_to_display = setup.xyz_to_display,
            .light_table    = true};
  }
  const auto setup     = bundle.Selection(negative, print_material, scene_light);
  const bool cine_look = bundle.PrintMaterial(print_material).cinema_look;
  return {.layer_gain          = setup.layer_gain,
          .xyz_to_display      = setup.xyz_to_display,
          .negative_grade_grey = setup.negative_grade_grey,
          .cinema_look         = cine_look,
          .negative_grade      = cine_look && bundle.Negative(negative).negative_grade};
}

/**
 * @brief The fixed values of a selection, for a `FilmKernelParams` of either form.
 *
 * The caller sets `display_encoding`, because the display curve belongs to the output and not to
 * the selection. For a colour reversal film @p print_material is not read, and the values are those
 * of the light table form.
 *
 * @throws std::runtime_error that names the identifier for an unknown selection.
 */
template <class Params>
void FillParams(const FilmStockBundle& bundle, std::string_view negative,
                std::string_view print_material, std::string_view scene_light,
                std::uint32_t print_edge, Params& params) {
  using Real           = std::remove_reference_t<decltype(params.gamut_power)>;
  const auto layout    = LayoutOf(bundle, negative);
  const auto stock     = bundle.Film(negative);
  const auto setup     = SelectionValuesOf(bundle, negative, print_material, scene_light);
  const auto constants = bundle.PointConstants();
  const auto nodes     = bundle.UvNodes();
  const auto real      = [](double value) { return static_cast<Real>(value); };

  bool has_image = false;
  bool has_point = false;
  for (std::size_t k = 0; k < 9; ++k) {
    params.ap1_to_xyz[k]     = real(constants.ap1_to_xyz[k]);
    params.xyz_to_display[k] = real(setup.xyz_to_display[k]);
    params.inhibition_k[k]   = stock.inhibition.Present() ? real(stock.inhibition.k[k]) : Real{};
    params.inhibition_image_k[k] =
        stock.inhibition.Present() ? real(stock.inhibition.image_k[k]) : Real{};
    params.inhibition_point_k[k] = stock.inhibition.Present()
                                       ? real(stock.inhibition.k[k] - stock.inhibition.image_k[k])
                                       : Real{};
    has_image                    = has_image || params.inhibition_image_k[k] != Real{};
    has_point                    = has_point || params.inhibition_point_k[k] != Real{};
  }
  params.locus_white[0]  = real(constants.locus_white[0]);
  params.locus_white[1]  = real(constants.locus_white[1]);
  params.locus_keep      = real(1.0 - constants.locus_inset);
  // The largest circle at the white where rate <= room * keep holds on each facet, with Y = 1.
  double inner_squared   = std::numeric_limits<double>::infinity();
  for (std::size_t facet = 0; facet < layout.facets; ++facet) {
    const double nx = constants.locus_normals[2 * facet];
    const double nz = constants.locus_normals[2 * facet + 1];
    const double reach =
        (std::max)(constants.locus_room[facet], 0.0) * (1.0 - constants.locus_inset);
    inner_squared = (std::min)(inner_squared, reach * reach / (nx * nx + nz * nz));
  }
  params.locus_inner_squared = real(inner_squared);
  params.uv_scale[0]     = real(static_cast<double>(nodes - 1) / constants.uv_max[0]);
  params.uv_scale[1]     = real(static_cast<double>(nodes - 1) / constants.uv_max[1]);
  params.curve_step      = real(stock.curves.log_h_step);
  params.inhibition_step = stock.inhibition.Present() ? real(stock.inhibition.log_h_step) : Real{1};
  const auto grid_step   = 1.0 / (static_cast<double>(print_edge) - 1.0);
  for (std::size_t k = 0; k < 3; ++k) {
    params.layer_gain[k]   = real(setup.layer_gain[k]);
    params.curve_offset[k] = real(stock.criterion_log_h[k] - stock.curves.log_h_start[k]);
    params.inhibition_offset[k] =
        stock.inhibition.Present()
            ? real(stock.criterion_log_h[k] - stock.inhibition.log_h_start[k])
            : Real{};
    params.print_start[k] = real(stock.density_grid_start[k]);
    params.print_step[k] =
        real((stock.density_grid_stop[k] - stock.density_grid_start[k]) * grid_step);
    params.curve_lengths[k] = static_cast<std::uint32_t>(stock.curves.lengths[k]);
    params.inhibition_lengths[k] =
        stock.inhibition.Present() ? static_cast<std::uint32_t>(stock.inhibition.lengths[k]) : 0U;
    // The constant scale of rule 'rgc_style_display_compression', from the limit and the threshold.
    const auto threshold      = constants.gamut_threshold[k];
    const auto span           = constants.gamut_limit[k] - threshold;
    params.gamut_threshold[k] = real(threshold);
    params.gamut_scale[k] =
        real(span / std::pow(std::pow((1.0 - threshold) / span, -constants.gamut_power) - 1.0,
                             1.0 / constants.gamut_power));
  }
  params.gamut_power              = real(constants.gamut_power);
  params.cinema_look              = setup.cinema_look ? 1U : 0U;
  params.negative_grade           = setup.negative_grade ? 1U : 0U;
  params.light_table              = setup.light_table ? 1U : 0U;
  const auto light_table          = bundle.LightTable();
  for (std::size_t k = 0; k < 3; ++k) {
    params.negative_grade_grey[k] = real(setup.negative_grade_grey[k]);
    params.display_luminance[k] =
        setup.light_table ? real(light_table.display_luminance[k]) : Real{};
  }
  // Lab rule 'dark_surround_on_display' acts on the print table of a print, and in display light on
  // a light table.
  params.display_surround_power =
      setup.light_table ? real(light_table.surround_exponent - 1.0) : Real{};
  params.inhibition_has_image     = has_image ? 1U : 0U;
  params.inhibition_has_point     = has_point ? 1U : 0U;
  params.locus_facets             = static_cast<std::uint32_t>(layout.facets);
  params.uv_nodes                 = static_cast<std::uint32_t>(nodes);
  params.curve_stored             = static_cast<std::uint32_t>(stock.curves.stored_length);
  params.inhibition_stored        = static_cast<std::uint32_t>(stock.inhibition.stored_length);
  params.print_edge               = print_edge;
  params.curves_offset            = static_cast<std::uint32_t>(layout.CurvesOffset());
  params.inhibition_values_offset = static_cast<std::uint32_t>(layout.InhibitionOffset());
  params.normals_offset           = static_cast<std::uint32_t>(layout.NormalsOffset());
  params.room_offset              = static_cast<std::uint32_t>(layout.RoomOffset());
}

}  // namespace alcedo::film_print_kernel_tables
