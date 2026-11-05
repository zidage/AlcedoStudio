//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
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

/**
 * @brief Values of one colour reversal film, lab rule 'removed_dye_record'.
 *
 * @ref record has the form of a negative: its curves give the removed dye, which rises with
 * exposure, so the steps of a negative read it with no change. The dye amount of a pixel is
 * `dye_max - record`. A reversal film has no negative grade.
 */
struct FilmReversalView {
  FilmNegativeView        record;
  std::span<const double> dye_max;                ///< (3)
  double                  clear_luminance = 0.0;  ///< Y of the clear film on the light table
};

/// Values of lab rule 'light_table_view' and of the dark surround in display light. They are the
/// same for all colour reversal films.
struct FilmLightTableConstants {
  std::span<const double> viewing_weights;    ///< (3, wavelengths): CIE 1931 under the table light
  std::span<const double> display_luminance;  ///< (3): Y of linear Rec. 709
  double                  xyz_scale         = 0.0;
  double                  surround_exponent = 1.0;  ///< On luminance in linear display light
};

/// One medium of rule 'generic_density_inversion' of Negative Scan (lab step P14).
struct FilmScanMedium {
  std::string              id;     ///< `photo` or `cine`
  std::span<const double>  gamma;  ///< (3): gamma of the medium for the default scanner
  /// The pool of the medium (lab rule 'generic_scan'), and the scene light of each pool stock: its
  /// balance light. With another scanner, the host derives gamma again from these stocks.
  std::vector<std::string> pool;
  std::vector<std::string> pool_scene_lights;
};

/**
 * @brief Values of Negative Scan that are the same for all negatives (lab step P14).
 *
 * The default scanner of lab rule 'ideal_scanner_bands': a Gaussian band per channel with these
 * centres and full width at half maximum, cut below @ref band_cut of its peak, under a Planckian
 * lamp at @ref lamp_kelvin (`halogen`) or an equal-energy light.
 */
struct FilmNegativeScanConstants {
  std::array<double, 3>       centres_nm{};  ///< R, G, B
  double                      width_nm       = 0.0;
  double                      band_cut       = 0.0;
  double                      lamp_kelvin    = 0.0;
  double                      planck_c2_nm_k = 0.0;  ///< c2 of the Planck law, in nm K
  std::string                 default_light;         ///< `halogen` or `equal_energy`
  /// Rule 'frame_auto_colour': the light axis is the change of a grey from the first light to the
  /// second (T3200 to D65). Each one is a spectrum of the bundle.
  std::array<std::string, 2>  axis_lights;
  std::vector<FilmScanMedium> media;
};

/// The fixed values of one selection of negative and scene light on Negative Scan, for the
/// default scanner.
struct FilmNegativeScanSetup {
  std::string           medium;         ///< The medium of the negative
  std::string           balance_light;  ///< Spectrum name of the balance light of the stock
  std::array<double, 3> layer_gain{};   ///< The layer gain of the print form of the same selection
  /// D_base: the scan density of the unexposed film.
  std::array<double, 3> base_density{};
  /// D_aim (lab rule 'stock_balance'): D - D_base of an 18 % grey under the balance light.
  std::array<double, 3> aim_density{};
  /// Rule 'frame_auto_colour': the change of x of an 18 % grey from T3200 to D65, contrast 1.
  std::array<double, 3> light_axis{};
  std::array<double, 3> gamma{};         ///< The gamma of the medium
  std::vector<double>   scene_spectrum;  ///< (wavelengths)
};

/**
 * @brief A scene light as the blend of two lights of the bundle: (1 - share) first + share second.
 *
 * A named light has the same light twice and a share of 0. Scene light `as_shot` blends the two
 * lights of the colour temperature grid around the colour temperature of the RAW decode, linear in
 * mired. Each light has Y = 1, and the layer exposure is linear in the light, so the u'v' table of
 * the blend is the blend of the two tables.
 */
struct FilmSceneLightMix {
  std::string first;
  std::string second;
  double      share = 0.0;  ///< Share of @ref second, in [0, 1]
};

/// The colour temperature grid of the scene lights, in the order of falling mired.
struct FilmCctGrid {
  std::vector<double>      kelvin;  ///< The rounded colour temperature of each light
  std::vector<std::string> lights;  ///< The scene light of each node
};

/// The fixed scalar values of one selection of colour reversal film and scene light.
struct FilmReversalSetup {
  std::array<double, 3> layer_gain{};      ///< Lab rule 'daylight_reversal_balance'
  std::array<double, 9> xyz_to_display{};  ///< Row major. Lab rule 'clear_film_white'
  std::vector<double>   scene_spectrum;    ///< (wavelengths)
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
  std::vector<double>     scene_spectrum;    ///< (wavelengths)
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
  /// @copydoc Negatives
  [[nodiscard]] auto Reversals() const -> std::span<const FilmStockName> { return reversals_; }
  /// The colour reversal film identifiers in the order of the lab list.
  [[nodiscard]] auto ReversalListOrder() const -> std::span<const std::string> {
    return reversal_order_;
  }
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
  /// The values under a blend of two lights: each value that depends on the light is the blend of
  /// its values under the two lights. @copydoc Selection
  [[nodiscard]] auto Selection(std::string_view negative, std::string_view print_material,
                               const FilmSceneLightMix& scene_light) const -> FilmSelectionSetup;

  /**
   * @brief The scene light of a document field, as lights of this bundle.
   *
   * @param scene_light A light of @ref SceneLights, or `as_shot`.
   * @param raw_cct The colour temperature of the RAW decode, or none for an input with no RAW
   *        white balance. Only `as_shot` reads it: with none, `as_shot` is the light `reference`;
   *        else the blend of the two grid lights around it, linear in mired. A temperature outside
   *        the grid takes the nearest end.
   * @throws std::runtime_error that names @p scene_light when it is unknown, and for a temperature
   *         that is not finite and above 0.
   */
  [[nodiscard]] auto SceneLightMix(std::string_view      scene_light,
                                   std::optional<double> raw_cct) const -> FilmSceneLightMix;
  [[nodiscard]] auto CctGrid() const -> const FilmCctGrid& { return cct_grid_; }

  /// True when @p id is a colour reversal film of this bundle.
  [[nodiscard]] auto IsReversal(std::string_view id) const -> bool;
  /// @throws std::runtime_error that names @p id when the reversal film is unknown.
  [[nodiscard]] auto Reversal(std::string_view id) const -> FilmReversalView;
  /// @throws std::runtime_error that names the identifier when one of the two is unknown.
  [[nodiscard]] auto ReversalSelection(std::string_view reversal,
                                       std::string_view scene_light) const -> FilmReversalSetup;
  /// @copydoc ReversalSelection
  [[nodiscard]] auto ReversalSelection(std::string_view         reversal,
                                       const FilmSceneLightMix& scene_light) const
      -> FilmReversalSetup;
  /**
   * @brief The values of the steps FP1 to FP5 of a film: of a negative, or the record of a colour
   * reversal film.
   * @throws std::runtime_error that names @p id when the film is unknown.
   */
  [[nodiscard]] auto Film(std::string_view id) const -> FilmNegativeView;
  [[nodiscard]] auto LightTable() const -> const FilmLightTableConstants& { return light_table_; }

  /// Print material Negative Scan: its identifier and name from the metadata `negative_scan`. It is
  /// not in @ref PrintMaterials, because it has no print.
  [[nodiscard]] auto NegativeScan() const -> const FilmStockName& { return negative_scan_; }
  /// True when @p id is the print material @ref NegativeScan.
  [[nodiscard]] auto IsNegativeScan(std::string_view id) const -> bool {
    return id == negative_scan_.id;
  }
  [[nodiscard]] auto NegativeScanConstants() const -> const FilmNegativeScanConstants& {
    return negative_scan_constants_;
  }
  /// The medium of @p id in @ref FilmNegativeScanConstants::media.
  /// @throws std::runtime_error that names @p id when the negative or its medium is unknown.
  [[nodiscard]] auto ScanMedium(std::string_view id) const -> const FilmScanMedium&;
  /// The spectrum name of the balance light of @p id for Negative Scan (D55 or T3200).
  /// @throws std::runtime_error that names @p id when the negative is unknown.
  [[nodiscard]] auto ScanBalanceLight(std::string_view id) const -> const std::string&;
  /// @throws std::runtime_error that names the identifier when one of the two is unknown.
  [[nodiscard]] auto NegativeScanSelection(std::string_view negative,
                                           std::string_view scene_light) const
      -> FilmNegativeScanSetup;
  /// @copydoc NegativeScanSelection
  [[nodiscard]] auto NegativeScanSelection(std::string_view         negative,
                                           const FilmSceneLightMix& scene_light) const
      -> FilmNegativeScanSetup;
  /// The layer gain of @p negative under the named light @p scene_light.
  [[nodiscard]] auto LayerGain(std::string_view negative, std::string_view scene_light) const
      -> std::array<double, 3>;
  /// The spectrum `light/<name>/spectrum` of the bundle (Y = 1).
  /// @throws std::runtime_error that names @p name when the bundle has no such spectrum.
  [[nodiscard]] auto LightSpectrum(std::string_view name) const -> std::span<const double>;

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
  /// The wavelengths of all spectral arrays, in nm.
  [[nodiscard]] auto WavelengthsNm() const -> std::span<const double> {
    return stock_.F64("common/wavelengths_nm", {wavelengths_});
  }

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
  void CheckExposureBand(std::string_view id, std::span<const double> exposure_weights) const;
  void ReadCctGrid();
  /// Index of a light of @ref SceneLights. @throws std::runtime_error that names an unknown one.
  [[nodiscard]] auto LightIndex(std::string_view scene_light) const -> std::int64_t;
  /// The spectrum of @p mix, from the spectra that row @p row of @p spectrum_index names per light.
  [[nodiscard]] auto MixSpectrum(std::span<const std::int32_t> spectrum_index, std::int64_t row,
                                 const FilmSceneLightMix& mix) const -> std::vector<double>;
  /// The values of group @p group ('negative' or 'reversal') with the form of a negative.
  [[nodiscard]] auto ReadFilmRecord(std::string_view group, std::string_view id) const
      -> FilmNegativeView;

  FilmArrayFile              stock_;
  FilmArrayFile              setup_;
  std::vector<float>         reflectance_rows_;
  std::vector<std::int32_t>  reflectance_nodes_;
  std::vector<FilmStockName> negatives_;
  std::vector<FilmStockName> papers_;
  std::vector<FilmStockName> reversals_;
  std::vector<std::string>   reversal_order_;
  std::vector<std::string>   scene_lights_;
  std::vector<std::string>   scene_spectra_;
  FilmStockName              negative_scan_;
  /// Per negative, in the order of @ref negatives_: the medium and the balance light of the scan.
  std::vector<std::string>   negative_media_;
  std::vector<std::string>   negative_balance_lights_;
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
  FilmLightTableConstants    light_table_;
  FilmNegativeScanConstants  negative_scan_constants_;
  FilmCctGrid                cct_grid_;
};

}  // namespace alcedo
