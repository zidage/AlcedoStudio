//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "edit/runtime/film/film_array_file.hpp"

namespace alcedo {

/// One film or print material in a list for the user.
struct FilmStockName {
  std::string id;
  std::string display_name;
};

/// Characteristic curves on one uniform log H step. A shorter curve repeats its last value.
struct FilmCurveView {
  std::span<const double>       density;      ///< (3, stored_length)
  std::span<const std::int64_t> lengths;      ///< (3)
  std::span<const double>       log_h_start;  ///< (3)
  std::int64_t                  stored_length = 0;
  double                        log_h_step    = 0.0;
};

/// Rule 'development_inhibition' of a negative. A negative with no modulation transfer data has
/// none.
struct FilmInhibitionView {
  std::span<const double>       k;            ///< (3, 3), row major: the total, which sets F_i
  std::span<const double>       image_k;      ///< (3, 3): the part of `k` with a blur of the image
  std::span<const double>       free_values;  ///< (3, stored_length): F_i on one uniform log H step
  std::span<const std::int64_t> lengths;      ///< (3)
  std::span<const double>       log_h_start;  ///< (3)
  std::int64_t                  stored_length = 0;
  double                        log_h_step    = 0.0;

  [[nodiscard]] auto            Present() const -> bool { return stored_length != 0; }
};

/// Values of one negative. All spans are (3, wavelengths) unless a comment states otherwise.
struct FilmNegativeView {
  std::span<const double> base;  ///< (wavelengths): density with no dye
  std::span<const double> dyes;
  std::span<const double> exposure_weights;    ///< Layer exposure: radiance @ weights^T
  std::span<const double> density_grid_start;  ///< (3): first node of each print table axis
  std::span<const double> density_grid_stop;   ///< (3): last node of each print table axis
  std::span<const double> criterion_log_h;     ///< (3): log H of a layer exposure of 1
  std::span<const double> layer_spread_um;     ///< (3): 0 for a negative with no transfer data
  FilmCurveView           curves;              ///< Dye amounts on log H
  FilmInhibitionView      inhibition;
  bool                    negative_grade = false;  ///< Lab rule negative_show_grade, on cine_look
};

/// Values of the point stages that are the same for all stocks. Lab rules
/// 'spectral_locus_projection' and 'rgc_style_display_compression'.
struct FilmPointConstants {
  std::span<const double> ap1_to_xyz;       ///< (3, 3), row major
  std::span<const double> locus_normals;    ///< (facets, 2)
  std::span<const double> locus_room;       ///< (facets)
  std::span<const double> locus_white;      ///< (2): X / Y and Z / Y of the white
  std::span<const double> uv_max;           ///< (2): u' and v' of the last table node
  std::span<const double> gamut_threshold;  ///< (3)
  std::span<const double> gamut_limit;      ///< (3)
  double                  locus_inset = 0.0;
  double                  gamut_power = 0.0;
};

/// Values of the image-plane steps that are the same for all stocks. Lab rules 'layer_spread',
/// 'development_inhibition', and the options of Halation and Enlarger flare.
struct FilmImagePlaneConstants {
  std::span<const double> halation_strength;          ///< (3): strength of each layer at slider 1
  std::span<const double> spread_gaussians;           ///< (2, 2): (weight, sigma / spread length)
  double                  frame_mm            = 0.0;  ///< Long side of the frame
  double                  halation_um         = 0.0;
  double                  inhibition_um       = 0.0;
  double                  enlarger_flare_size = 0.0;  ///< Sigma, as a share of the long side
};

/// Values of one print material and its viewing light. Spans are (3, wavelengths) unless a comment
/// states otherwise.
struct FilmPaperView {
  std::span<const double> base;  ///< (wavelengths): density with no dye
  std::span<const double> dyes;
  std::span<const double> printing_weights;
  std::span<const double> status_a_weights;
  std::span<const double> viewing_weights;
  std::span<const double> white_xyz;          ///< (3)
  std::span<const double> status_axis_start;  ///< (3)
  std::span<const double> status_axis_range;  ///< (3)
  FilmCurveView           curves;
  double                  xyz_scale       = 0.0;
  double                  clear_luminance = 0.0;  ///< Y of the clear print under the viewing light
  double                  surround_exponent = 1.0;    ///< Lab rule dark_surround_on_display
  bool                    cinema_look       = false;  ///< Lab rule print_cinema_look
};

/// The fixed scalar values of one selection of film, print material, and scene light.
struct FilmSelectionSetup {
  std::array<double, 3>   printer_balance_log10{};
  std::array<double, 3>   negative_grade_grey{};  ///< Record of an 18 % grey. 0: no such rule
  std::array<double, 3>   layer_gain{};
  std::array<double, 3>   printing_gain{};
  std::array<double, 3>   aim_log_h{};
  std::array<double, 9>   xyz_to_display{};  ///< Row major: XYZ to linear Rec. 709
  std::span<const double> scene_spectrum;    ///< (wavelengths)
};

/**
 * @brief The film print resources of one directory. Read-only after @ref Open.
 *
 * Owner of the stock bundle, the print setup, and the reflectance table. The reflectance table
 * holds about 25 MB for the life of the object. All methods are safe to call from any thread,
 * because nothing changes after @ref Open. Spans that the methods return stay valid for the life
 * of the object.
 */
class FilmStockBundle {
 public:
  /**
   * @brief Read and check the resources of a directory.
   *
   * Reads `manifest.json` and each file that it names, and checks the schema version, the size,
   * and the SHA-256 of each file and the shape of each array. Writes nothing.
   *
   * @param directory The directory that holds `manifest.json`.
   * @return The read-only bundle.
   * @throws std::runtime_error that names the file when a file is missing or damaged. No partial
   *         result stays in memory.
   */
  [[nodiscard]] static auto Open(const std::filesystem::path& directory)
      -> std::shared_ptr<const FilmStockBundle>;

  /// Lists in the order of their identifiers.
  [[nodiscard]] auto Negatives() const -> std::span<const FilmStockName> { return negatives_; }
  /// @copydoc Negatives
  [[nodiscard]] auto PrintMaterials() const -> std::span<const FilmStockName> { return papers_; }
  /// Scene light identifiers in the order of the catalog.
  [[nodiscard]] auto SceneLights() const -> std::span<const std::string> { return scene_lights_; }

  /// SHA-256 of the digests of all resources: the identity of the content of this bundle.
  [[nodiscard]] auto ContentDigest() const -> const std::string& { return content_digest_; }

  /// @throws std::runtime_error that names @p id when the negative is unknown.
  [[nodiscard]] auto Negative(std::string_view id) const -> FilmNegativeView;
  /// @throws std::runtime_error that names @p id when the print material is unknown.
  [[nodiscard]] auto PrintMaterial(std::string_view id) const -> FilmPaperView;
  /// @throws std::runtime_error that names the identifier when one of the three is unknown.
  [[nodiscard]] auto Selection(std::string_view negative, std::string_view print_material,
                               std::string_view scene_light) const -> FilmSelectionSetup;

  /**
   * @brief Reflectance shapes of the kept u'v' nodes: (kept nodes, reflectance wavelengths).
   *
   * The file holds only the wavelengths where a weight of a negative is not zero, and only the
   * nodes that a bilinear read can reach after the spectral locus projection. Values have 12
   * mantissa bits.
   */
  [[nodiscard]] auto ReflectanceRows() const -> std::span<const float> { return reflectance_rows_; }
  /// For each row: the node index `u * uv_nodes + v`, in rising order.
  [[nodiscard]] auto ReflectanceNodes() const -> std::span<const std::int32_t> {
    return reflectance_nodes_;
  }
  /// Index in the complete wavelength list of the first column of @ref ReflectanceRows.
  [[nodiscard]] auto ReflectanceFirstWavelength() const -> std::int64_t {
    return reflectance_first_wavelength_;
  }
  /// Column count of @ref ReflectanceRows.
  [[nodiscard]] auto ReflectanceWavelengths() const -> std::int64_t {
    return reflectance_wavelengths_;
  }
  [[nodiscard]] auto UvNodes() const -> std::int64_t { return uv_nodes_; }
  [[nodiscard]] auto Wavelengths() const -> std::int64_t { return wavelengths_; }

  [[nodiscard]] auto PrintContrast() const -> double { return print_contrast_; }
  [[nodiscard]] auto PrintFlash() const -> double { return print_flash_; }
  [[nodiscard]] auto ViewingFlare() const -> double { return viewing_flare_; }
  [[nodiscard]] auto PointConstants() const -> const FilmPointConstants& {
    return point_constants_;
  }
  [[nodiscard]] auto ImagePlaneConstants() const -> const FilmImagePlaneConstants& {
    return image_plane_constants_;
  }

 private:
  FilmStockBundle(FilmArrayFile stock, FilmArrayFile setup, const FilmArrayFile& reflectance,
                  std::string content_digest);

  void                       ReadLists();
  void                       ReadCommonValues();
  void                       ReadReflectance(const FilmArrayFile& file);
  void                       CheckShapes() const;

  FilmArrayFile              stock_;
  FilmArrayFile              setup_;
  std::vector<float>         reflectance_rows_;
  std::vector<std::int32_t>  reflectance_nodes_;
  std::vector<FilmStockName> negatives_;
  std::vector<FilmStockName> papers_;
  std::vector<std::string>   scene_lights_;
  std::vector<std::string>   scene_spectra_;
  std::string                content_digest_;
  std::int64_t               reflectance_first_wavelength_ = 0;
  std::int64_t               reflectance_wavelengths_      = 0;
  std::int64_t               uv_nodes_                     = 0;
  std::int64_t               wavelengths_                  = 0;
  double                     print_contrast_               = 0.0;
  double                     print_flash_                  = 0.0;
  double                     viewing_flare_                = 0.0;
  FilmPointConstants         point_constants_;
  FilmImagePlaneConstants    image_plane_constants_;
};

}  // namespace alcedo
