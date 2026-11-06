//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/film/film_stock_bundle.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <json.hpp>
#include <stdexcept>
#include <utility>

#include "edit/runtime/film/film_grain_field.h"
#include "utils/hash/sha256.hpp"

namespace alcedo {
namespace {

constexpr std::string_view kManifestFileName = "manifest.json";

/// The source tiers of the grain data (plan 'film_grain_format_and_stock_plan.md', section 4.2).
constexpr std::array<std::string_view, 5> kFilmGrainTiers{"curve", "number", "family", "pgi",
                                                          "reversal"};

[[noreturn]] void          FailFile(const std::filesystem::path& path, std::string_view reason) {
  throw std::runtime_error("Film print resource '" + FilmPathText(path) +
                                    "': " + std::string(reason));
}

auto ReadManifest(const std::filesystem::path& path) -> nlohmann::json {
  const auto  bytes = ReadFilmResourceBytes(path);
  const auto* first = reinterpret_cast<const char*>(bytes.data());
  try {
    auto manifest = nlohmann::json::parse(first, first + bytes.size());
    if (manifest.at("schema_version").get<int>() != kFilmResourceSchemaVersion) {
      FailFile(path, "schema version " + manifest.at("schema_version").dump() + " is not version " +
                         std::to_string(kFilmResourceSchemaVersion));
    }
    return manifest;
  } catch (const nlohmann::json::exception& error) {
    FailFile(path, std::string("the manifest is not valid: ") + error.what());
  }
}

struct CheckedResource {
  FilmArrayFile file;
  std::string   sha256;
};

auto ReadResource(const std::filesystem::path& directory, const nlohmann::json& manifest,
                  const char* key) -> CheckedResource {
  std::string   file_name;
  std::string   expected_sha256;
  std::uint64_t expected_bytes = 0;
  try {
    const auto& entry = manifest.at("resources").at(key);
    file_name         = entry.at("file").get<std::string>();
    expected_sha256   = entry.at("sha256").get<std::string>();
    expected_bytes    = entry.at("bytes").get<std::uint64_t>();
  } catch (const nlohmann::json::exception& error) {
    FailFile(directory / kManifestFileName,
             "the entry '" + std::string(key) + "' is not valid: " + error.what());
  }
  const auto path =
      directory / std::filesystem::path(std::u8string(
                      reinterpret_cast<const char8_t*>(file_name.data()), file_name.size()));
  auto bytes = ReadFilmResourceBytes(path);
  if (bytes.size() != expected_bytes) {
    FailFile(path, "the file has " + std::to_string(bytes.size()) + " bytes; the manifest states " +
                       std::to_string(expected_bytes));
  }
  auto sha256 = ComputeSha256Hex(bytes);
  if (sha256 != expected_sha256) {
    FailFile(path, "SHA-256 " + sha256 + " is not the manifest value " + expected_sha256);
  }
  return {FilmArrayFile::Parse(std::move(bytes), path), std::move(sha256)};
}

auto IndexOf(std::span<const FilmStockName> list, std::string_view id, std::string_view kind)
    -> std::int64_t {
  const auto entry = std::lower_bound(
      list.begin(), list.end(), id,
      [](const FilmStockName& name, std::string_view value) { return name.id < value; });
  if (entry == list.end() || entry->id != id) {
    throw std::runtime_error("Film print: unknown " + std::string(kind) + " '" + std::string(id) +
                             "'");
  }
  return entry - list.begin();
}

auto ArrayName(std::string_view group, std::string_view id, std::string_view field) -> std::string {
  std::string name(group);
  name += '/';
  name += id;
  name += '/';
  name += field;
  return name;
}

template <std::size_t Count>
auto CopyValues(std::span<const double> values, std::int64_t first) -> std::array<double, Count> {
  std::array<double, Count> out{};
  std::copy_n(values.begin() + first, Count, out.begin());
  return out;
}

/// The values of row @p row under the two lights of a mix, (1 - share) first + share second.
/// @p lights is the light count, and a row holds `lights * Count` values.
template <std::size_t Count>
auto MixValues(std::span<const double> values, std::int64_t row, std::int64_t lights,
               std::int64_t first, std::int64_t second, double share) -> std::array<double, Count> {
  const auto a =
      CopyValues<Count>(values, (row * lights + first) * static_cast<std::int64_t>(Count));
  if (share == 0.0) {
    return a;
  }
  const auto b =
      CopyValues<Count>(values, (row * lights + second) * static_cast<std::int64_t>(Count));
  std::array<double, Count> out{};
  for (std::size_t k = 0; k < Count; ++k) {
    out[k] = (1.0 - share) * a[k] + share * b[k];
  }
  return out;
}

auto SingleLight(std::string_view scene_light) -> FilmSceneLightMix {
  return {std::string(scene_light), std::string(scene_light), 0.0};
}

void RequireCurveLengths(std::span<const std::int64_t> lengths, std::int64_t stored_length,
                         std::string_view id) {
  if (std::ranges::any_of(lengths, [stored_length](std::int64_t length) {
        return length < 2 || length > stored_length;
      })) {
    throw std::runtime_error("Film print resource: the curves of '" + std::string(id) +
                             "' have an invalid length");
  }
}

/// The characteristic curves of a negative or a paper. @p field gives the name of an array.
template <class Field>
auto ReadCurves(const FilmArrayFile& stock, const Field& field, std::string_view id)
    -> FilmCurveView {
  const auto shape = stock.Shape(field("curve_density"));
  if (shape.size() != 2) {
    throw std::runtime_error("Film print resource: the curves of '" + std::string(id) +
                             "' do not have two axes");
  }
  const auto lengths = stock.I64(field("curve_lengths"), {3});
  RequireCurveLengths(lengths, shape[1], id);
  return {
      .density       = stock.F64(field("curve_density"), {3, shape[1]}),
      .lengths       = lengths,
      .log_h_start   = stock.F64(field("curve_log_h_start"), {3}),
      .stored_length = shape[1],
      .log_h_step    = stock.F64(field("curve_log_h_step"), {})[0],
  };
}

/// The curves `grain/<id>/<x_name>` and `<sigma_name>` with the counts `<count_name>`.
auto ReadGrainCurves(const FilmArrayFile& stock, std::string_view id, std::string_view x_name,
                     std::string_view sigma_name, std::string_view count_name)
    -> FilmGrainCurveView {
  const auto shape = stock.Shape(ArrayName("grain", id, x_name));
  if (shape.size() != 2 || shape[0] != 3 || shape[1] < 2) {
    throw std::runtime_error("Film print resource: the grain curves '" + std::string(x_name) +
                             "' of '" + std::string(id) + "' do not have the shape (3, n)");
  }
  return {
      .x            = stock.F64(ArrayName("grain", id, x_name), {3, shape[1]}),
      .sigma        = stock.F64(ArrayName("grain", id, sigma_name), {3, shape[1]}),
      .node_count   = stock.I32(ArrayName("grain", id, count_name), {3}),
      .stored_nodes = shape[1],
  };
}

/// Each counted curve has 2 to FILM_GRAIN_CURVE_NODES nodes, a rising x, and a sigma above 0.
void CheckGrainCurves(const FilmGrainCurveView& curves, std::string_view id,
                      std::string_view group) {
  const auto fail = [&](std::size_t layer, const std::string& reason) {
    throw std::runtime_error("Film print resource: the grain curve '" + std::string(group) + "' " +
                             std::to_string(layer) + " of '" + std::string(id) + "' " + reason);
  };
  for (std::size_t layer = 0; layer < 3; ++layer) {
    const auto count = curves.node_count[layer];
    if (count < 2 || count > curves.stored_nodes || count > FILM_GRAIN_CURVE_NODES) {
      fail(layer, "has " + std::to_string(count) + " nodes");
    }
    const auto first = static_cast<std::size_t>(curves.stored_nodes) * layer;
    for (std::size_t node = 0; node < static_cast<std::size_t>(count); ++node) {
      const auto x     = curves.x[first + node];
      const auto sigma = curves.sigma[first + node];
      if (!std::isfinite(x) || !std::isfinite(sigma) || !(sigma > 0.0)) {
        fail(layer, "has a node that is not finite or a sigma that is not above 0");
      }
      if (node > 0 && !(x > curves.x[first + node - 1])) {
        fail(layer, "does not rise");
      }
    }
  }
}

}  // namespace

auto FilmStockBundle::Open(const std::filesystem::path& directory)
    -> std::shared_ptr<const FilmStockBundle> {
  const auto manifest = ReadManifest(directory / kManifestFileName);
  auto       stock    = ReadResource(directory, manifest, "stock_bundle");
  auto       setup    = ReadResource(directory, manifest, "print_setup");
  auto       table    = ReadResource(directory, manifest, "reflectance_table");
  const auto digests  = stock.sha256 + setup.sha256 + table.sha256;
  return std::shared_ptr<const FilmStockBundle>(
      new FilmStockBundle(std::move(stock.file), std::move(setup.file), table.file,
                          ComputeSha256Hex(std::as_bytes(std::span(digests)))));
}

FilmStockBundle::FilmStockBundle(FilmArrayFile stock, FilmArrayFile setup,
                                 const FilmArrayFile& reflectance, std::string content_digest)
    : stock_(std::move(stock)),
      setup_(std::move(setup)),
      content_digest_(std::move(content_digest)) {
  ReadLists();
  ReadCctGrid();
  ReadCommonValues();
  ReadReflectance(reflectance);
  ReadGrain();
  CheckShapes();
}

void FilmStockBundle::ReadGrain() {
  grain_constants_ = {
      .cloud_sigma_um           = stock_.F64("common/grain_cloud_sigma_um", {3}),
      .layer_correlation        = stock_.F64("common/grain_layer_correlation", {3}),
      .post_display_value       = stock_.F64("common/grain_post_display_value", {3}),
      .post_display_sigma_48    = stock_.F64("common/grain_post_display_sigma_48", {3}),
      .post_display_correlation = stock_.F64("common/grain_post_display_correlation", {3}),
      .aperture_area_um2        = stock_.F64("common/grain_aperture_area_um2", {})[0],
  };
  const auto& common      = grain_constants_;
  const auto  above_zero  = [](double value) { return std::isfinite(value) && value > 0.0; };
  const auto  correlation = [](double value) { return std::isfinite(value) && std::abs(value) < 1.0; };
  if (!std::ranges::all_of(common.cloud_sigma_um, above_zero) ||
      !std::ranges::all_of(common.post_display_value, above_zero) ||
      !std::ranges::all_of(common.post_display_sigma_48, above_zero) ||
      !above_zero(common.aperture_area_um2)) {
    throw std::runtime_error(
        "Film print resource: a cloud size, the aperture, or a post target of the grain is not "
        "above 0");
  }
  if (!std::ranges::all_of(common.layer_correlation, correlation) ||
      !std::ranges::all_of(common.post_display_correlation, correlation)) {
    throw std::runtime_error(
        "Film print resource: a correlation of the grain is not finite and inside (-1, 1)");
  }

  std::vector<std::string_view> films;
  for (const auto& negative : negatives_) {
    films.emplace_back(negative.id);
  }
  for (const auto& reversal : reversals_) {
    films.emplace_back(reversal.id);
  }
  grain_tiers_.clear();
  for (const auto id : films) {
    if (!stock_.Contains(ArrayName("grain", id, "layer_amount"))) {
      throw std::runtime_error("Film print resource: the film '" + std::string(id) +
                               "' has no grain group");
    }
    std::string tier;
    try {
      tier = stock_.Metadata().at("grain").at(std::string(id)).at("tier").get<std::string>();
    } catch (const nlohmann::json::exception& error) {
      throw std::runtime_error("Film print resource: the grain metadata of '" + std::string(id) +
                               "' is not valid: " + error.what());
    }
    if (std::ranges::find(kFilmGrainTiers, tier) == kFilmGrainTiers.end()) {
      throw std::runtime_error("Film print resource: the grain tier '" + tier + "' of '" +
                               std::string(id) + "' is unknown");
    }
    grain_tiers_.push_back(std::move(tier));
    CheckGrainCurves(
        ReadGrainCurves(stock_, id, "layer_amount", "layer_sigma_amount", "layer_node_count"), id,
        "layer");
    CheckGrainCurves(
        ReadGrainCurves(stock_, id, "channel_density", "channel_sigma_48", "channel_node_count"),
        id, "channel");
  }
}

auto FilmStockBundle::Grain(std::string_view id) const -> FilmGrainStockView {
  const bool reversal = IsReversal(id);
  const auto index    = reversal ? static_cast<std::int64_t>(negatives_.size()) +
                                    IndexOf(reversals_, id, "reversal film")
                                 : IndexOf(negatives_, id, "film");
  return {
      .layer =
          ReadGrainCurves(stock_, id, "layer_amount", "layer_sigma_amount", "layer_node_count"),
      .channel = ReadGrainCurves(stock_, id, "channel_density", "channel_sigma_48",
                                 "channel_node_count"),
      .common   = grain_constants_,
      .tier     = grain_tiers_[static_cast<std::size_t>(index)],
      .reversal = reversal,
  };
}

void FilmStockBundle::ReadLists() {
  const auto read_names = [this](const char* key) {
    std::vector<FilmStockName> names;
    for (const auto& [id, entry] : stock_.Metadata().at(key).items()) {
      names.push_back({id, entry.at("name").get<std::string>()});
    }
    return names;
  };
  try {
    negatives_      = read_names("negatives");
    papers_         = read_names("print_materials");
    reversals_      = read_names("reversal_films");
    reversal_order_ = stock_.Metadata().at("reversal_list_order").get<std::vector<std::string>>();
    scene_lights_   = stock_.Metadata().at("scene_lights").get<std::vector<std::string>>();
    scene_spectra_  = stock_.Metadata().at("scene_spectra").get<std::vector<std::string>>();
    for (const auto& negative : negatives_) {
      const auto& entry = stock_.Metadata().at("negatives").at(negative.id);
      negative_media_.push_back(entry.at("medium").get<std::string>());
      negative_balance_lights_.push_back(entry.at("scan_balance_light").get<std::string>());
    }
    const auto& negative_scan = stock_.Metadata().at("negative_scan");
    negative_scan_ = {negative_scan.at("id").get<std::string>(),
                      negative_scan.at("name").get<std::string>()};
    auto& constants         = negative_scan_constants_;
    constants.default_light = negative_scan.at("light").get<std::string>();
    const auto axis_lights  = negative_scan.at("axis_lights").get<std::vector<std::string>>();
    if (axis_lights.size() != 2) {
      throw std::runtime_error("Film print resource: Negative Scan does not have two axis lights");
    }
    constants.axis_lights = {axis_lights[0], axis_lights[1]};
    for (const auto& [id, entry] : negative_scan.at("media").items()) {
      FilmScanMedium medium;
      medium.id                = id;
      medium.pool              = entry.at("pool").get<std::vector<std::string>>();
      medium.pool_scene_lights = entry.at("pool_scene_lights").get<std::vector<std::string>>();
      constants.media.push_back(std::move(medium));
    }
  } catch (const nlohmann::json::exception& error) {
    throw std::runtime_error(std::string("Film print resource: the stock lists are not valid: ") +
                             error.what());
  }
  if (negatives_.empty() || papers_.empty() || reversals_.empty() || scene_lights_.empty()) {
    throw std::runtime_error("Film print resource: a stock list is empty");
  }
  // Negative Scan is a value of the field of the print materials, so it must not name one of them.
  if (negative_scan_.id.empty() ||
      std::ranges::any_of(papers_, [this](const FilmStockName& paper) {
        return paper.id == negative_scan_.id;
      })) {
    throw std::runtime_error("Film print resource: the identifier of Negative Scan '" +
                             negative_scan_.id + "' is empty or names a print material");
  }
  const auto& media = negative_scan_constants_.media;
  for (const auto& medium : media) {
    if (medium.pool.empty() || medium.pool.size() != medium.pool_scene_lights.size()) {
      throw std::runtime_error("Film print resource: the pool of the scan medium '" + medium.id +
                               "' is empty or has a light list of another size");
    }
  }
  for (std::size_t n = 0; n < negatives_.size(); ++n) {
    if (std::ranges::none_of(media, [&](const FilmScanMedium& medium) {
          return medium.id == negative_media_[n];
        })) {
      throw std::runtime_error("Film print resource: the medium '" + negative_media_[n] + "' of '" +
                               negatives_[n].id + "' has no Negative Scan values");
    }
  }
  // The setup indexes a film by its place in the list of identifiers, so the two lists must not
  // share an identifier, and the list order must name each reversal film one time.
  for (const auto& reversal : reversals_) {
    if (std::ranges::any_of(negatives_, [&reversal](const FilmStockName& negative) {
          return negative.id == reversal.id;
        })) {
      throw std::runtime_error("Film print resource: '" + reversal.id +
                               "' is a negative and a reversal film");
    }
  }
  auto order = reversal_order_;
  std::ranges::sort(order);
  if (!std::ranges::equal(order, reversals_, {}, {}, &FilmStockName::id)) {
    throw std::runtime_error(
        "Film print resource: the list order of the reversal films does not name each film once");
  }
}

void FilmStockBundle::ReadCommonValues() {
  const auto wavelengths = stock_.Shape("common/wavelengths_nm");
  if (wavelengths.size() != 1) {
    throw std::runtime_error("Film print resource: the wavelength list does not have one axis");
  }
  wavelengths_           = wavelengths[0];
  print_contrast_        = stock_.F64("common/print_contrast", {})[0];
  print_flash_           = stock_.F64("common/print_flash", {})[0];
  viewing_flare_         = stock_.F64("common/viewing_flare", {})[0];
  image_plane_constants_ = {
      .halation_strength   = stock_.F64("common/halation_strength", {3}),
      .spread_gaussians    = stock_.F64("common/spread_gaussians", {2, 2}),
      .frame_mm            = stock_.F64("common/frame_mm", {})[0],
      .halation_um         = stock_.F64("common/halation_um", {})[0],
      .inhibition_um       = stock_.F64("common/inhibition_um", {})[0],
      .enlarger_flare_size = stock_.F64("common/enlarger_flare_size", {})[0],
  };
  light_table_ = {
      .viewing_weights   = stock_.F64("light_table/viewing_weights", {3, wavelengths_}),
      .display_luminance = stock_.F64("light_table/display_luminance", {3}),
      .xyz_scale         = stock_.F64("light_table/xyz_scale", {})[0],
      .surround_exponent = stock_.F64("light_table/surround_exponent", {})[0],
  };
  auto& scan          = negative_scan_constants_;
  const auto centres  = stock_.F64("common/negative_scan_centres_nm", {3});
  scan.centres_nm     = {centres[0], centres[1], centres[2]};
  scan.width_nm       = stock_.F64("common/negative_scan_width_nm", {})[0];
  scan.band_cut       = stock_.F64("common/negative_scan_band_cut", {})[0];
  scan.lamp_kelvin    = stock_.F64("common/negative_scan_lamp_kelvin", {})[0];
  scan.planck_c2_nm_k = stock_.F64("common/negative_scan_planck_c2_nm_k", {})[0];
  if (!(scan.width_nm > 0.0) || !(scan.band_cut > 0.0) || !(scan.lamp_kelvin > 0.0) ||
      !(scan.planck_c2_nm_k > 0.0)) {
    throw std::runtime_error("Film print resource: the scanner of Negative Scan is not valid");
  }
  for (auto& medium : scan.media) {
    medium.gamma = stock_.F64(ArrayName("scan_medium", medium.id, "gamma"), {3});
    if (std::ranges::any_of(medium.gamma,
                            [](double value) { return !(value > 0.0) || !std::isfinite(value); })) {
      throw std::runtime_error("Film print resource: a gamma of the scan medium '" + medium.id +
                               "' is not above 0");
    }
  }
  const auto normals = stock_.Shape("common/locus_normals");
  if (normals.size() != 2 || normals[1] != 2 || normals[0] < 1) {
    throw std::runtime_error("Film print resource: the locus normals do not have the shape (n, 2)");
  }
  point_constants_ = {
      .ap1_to_xyz      = stock_.F64("common/ap1_to_xyz", {3, 3}),
      .locus_normals   = stock_.F64("common/locus_normals", {normals[0], 2}),
      .locus_room      = stock_.F64("common/locus_room", {normals[0]}),
      .locus_white     = stock_.F64("common/locus_white", {2}),
      .uv_max          = stock_.F64("common/uv_max", {2}),
      .gamut_threshold = stock_.F64("common/gamut_threshold", {3}),
      .gamut_limit     = stock_.F64("common/gamut_limit", {3}),
      .locus_inset     = stock_.F64("common/locus_inset", {})[0],
      .gamut_power     = stock_.F64("common/gamut_power", {})[0],
  };
}

void FilmStockBundle::ReadReflectance(const FilmArrayFile& file) {
  const auto fail = [&file](const std::string& reason) {
    throw std::runtime_error("Film print resource '" + file.PathText() + "': " + reason);
  };
  uv_nodes_                     = file.I32("uv_nodes", {})[0];
  reflectance_first_wavelength_ = file.I32("first_wavelength", {})[0];
  reflectance_wavelengths_      = file.I32("wavelength_count", {})[0];
  if (uv_nodes_ < 2 || reflectance_first_wavelength_ < 0 || reflectance_wavelengths_ < 1 ||
      reflectance_first_wavelength_ + reflectance_wavelengths_ > wavelengths_) {
    fail("the table has an invalid node count or wavelength band");
  }
  const auto node_shape = file.Shape("node_index");
  if (node_shape.size() != 1 || node_shape[0] < 1) {
    fail("the node list does not have one axis");
  }
  const auto nodes = file.I32("node_index", {node_shape[0]});
  const auto last  = uv_nodes_ * uv_nodes_;
  for (std::size_t row = 0; row < nodes.size(); ++row) {
    if (nodes[row] < 0 || nodes[row] >= last || (row > 0 && nodes[row] <= nodes[row - 1])) {
      fail("the node list does not rise inside the table");
    }
  }
  const auto values = node_shape[0] * reflectance_wavelengths_;
  const auto planes = file.U8("byte_planes", {4, values});
  reflectance_nodes_.assign(nodes.begin(), nodes.end());
  reflectance_rows_.resize(static_cast<std::size_t>(values));
  // Each byte of a little-endian float32 is in its own plane, for the compression of the installer.
  auto* const out = reinterpret_cast<std::uint8_t*>(reflectance_rows_.data());
  for (std::size_t plane = 0; plane < sizeof(float); ++plane) {
    const auto* const in = planes.data() + plane * reflectance_rows_.size();
    for (std::size_t value = 0; value < reflectance_rows_.size(); ++value) {
      out[value * sizeof(float) + plane] = in[value];
    }
  }
}

void FilmStockBundle::ReadCctGrid() {
  try {
    const auto& grid = stock_.Metadata().at("cct_grid");
    cct_grid_.kelvin = grid.at("kelvin").get<std::vector<double>>();
    cct_grid_.lights = grid.at("lights").get<std::vector<std::string>>();
  } catch (const nlohmann::json::exception& error) {
    throw std::runtime_error(
        std::string("Film print resource: the colour temperature grid is not valid: ") +
        error.what());
  }
  if (cct_grid_.kelvin.size() < 2 || cct_grid_.kelvin.size() != cct_grid_.lights.size()) {
    throw std::runtime_error(
        "Film print resource: the colour temperature grid has fewer than 2 lights or a name list "
        "of "
        "another size");
  }
  for (std::size_t node = 0; node < cct_grid_.kelvin.size(); ++node) {
    if (!(cct_grid_.kelvin[node] > 0.0) ||
        (node > 0 && !(cct_grid_.kelvin[node] > cct_grid_.kelvin[node - 1]))) {
      throw std::runtime_error(
          "Film print resource: the colour temperature grid does not rise above 0");
    }
    (void)LightIndex(cct_grid_.lights[node]);
  }
}

auto FilmStockBundle::LightIndex(std::string_view scene_light) const -> std::int64_t {
  const auto light = std::find(scene_lights_.begin(), scene_lights_.end(), scene_light);
  if (light == scene_lights_.end()) {
    throw std::runtime_error("Film print: unknown scene light '" + std::string(scene_light) + "'");
  }
  return light - scene_lights_.begin();
}

auto FilmStockBundle::SceneLightMix(std::string_view      scene_light,
                                    std::optional<double> raw_cct) const -> FilmSceneLightMix {
  if (scene_light != "as_shot") {
    (void)LightIndex(scene_light);
    return SingleLight(scene_light);
  }
  // An input with no RAW white balance reads As Shot as the reference (user rule of issue 661).
  if (!raw_cct.has_value()) {
    return SingleLight("reference");
  }
  if (!std::isfinite(*raw_cct) || !(*raw_cct > 0.0)) {
    throw std::runtime_error(
        "Film print: the colour temperature of scene light As Shot is not "
        "finite and above 0");
  }
  // Lab grid_blend: the nodes are the mired of the rounded kelvin of each light, and the share is
  // linear in mired.
  const auto& kelvin = cct_grid_.kelvin;
  const auto& lights = cct_grid_.lights;
  const auto  mired  = 1e6 / *raw_cct;
  if (mired >= 1e6 / kelvin.front()) {
    return SingleLight(lights.front());
  }
  if (mired <= 1e6 / kelvin.back()) {
    return SingleLight(lights.back());
  }
  for (std::size_t k = 0; k + 1 < kelvin.size(); ++k) {
    const auto high = 1e6 / kelvin[k];
    const auto low  = 1e6 / kelvin[k + 1];
    if (high >= mired && mired >= low) {
      return {lights[k], lights[k + 1], (high - mired) / (high - low)};
    }
  }
  throw std::runtime_error("Film print: the colour temperature grid has no node pair for " +
                           std::to_string(*raw_cct) + " K");
}

auto FilmStockBundle::MixSpectrum(std::span<const std::int32_t> spectrum_index, std::int64_t row,
                                  const FilmSceneLightMix& mix) const -> std::vector<double> {
  const auto lights   = static_cast<std::int64_t>(scene_lights_.size());
  const auto spectrum = [&](std::string_view light) {
    const auto  index = spectrum_index[row * lights + LightIndex(light)];
    const auto& name  = scene_spectra_[static_cast<std::size_t>(index)];
    return stock_.F64(ArrayName("light", name, "spectrum"), {wavelengths_});
  };
  const auto          first = spectrum(mix.first);
  std::vector<double> out(first.begin(), first.end());
  if (mix.share != 0.0) {
    const auto second = spectrum(mix.second);
    for (std::size_t w = 0; w < out.size(); ++w) {
      out[w] = (1.0 - mix.share) * first[w] + mix.share * second[w];
    }
  }
  return out;
}

void FilmStockBundle::CheckExposureBand(std::string_view        id,
                                        std::span<const double> exposure_weights) const {
  for (std::size_t index = 0; index < exposure_weights.size(); ++index) {
    const auto wavelength = static_cast<std::int64_t>(index) % wavelengths_;
    if (exposure_weights[index] != 0.0 &&
        (wavelength < reflectance_first_wavelength_ ||
         wavelength >= reflectance_first_wavelength_ + reflectance_wavelengths_)) {
      throw std::runtime_error("Film print resource: the exposure weights of '" + std::string(id) +
                               "' are outside the wavelength band of the reflectance table");
    }
  }
}

void FilmStockBundle::CheckShapes() const {
  for (const auto& negative : negatives_) {
    CheckExposureBand(negative.id, Negative(negative.id).exposure_weights);
  }
  for (const auto& reversal : reversals_) {
    CheckExposureBand(reversal.id, Reversal(reversal.id).record.exposure_weights);
  }
  for (const auto& paper : papers_) {
    (void)PrintMaterial(paper.id);
  }
  for (const auto& spectrum : scene_spectra_) {
    (void)stock_.F64(ArrayName("light", spectrum, "spectrum"), {wavelengths_});
  }
  const auto negatives = static_cast<std::int64_t>(negatives_.size());
  const auto papers    = static_cast<std::int64_t>(papers_.size());
  const auto lights    = static_cast<std::int64_t>(scene_lights_.size());
  (void)setup_.F64("printer_balance_log10", {negatives, papers, lights, 3});
  (void)setup_.F64("layer_gain", {negatives, lights, 3});
  (void)setup_.F64("printing_gain", {negatives, papers, 3});
  (void)setup_.F64("aim_log_h", {papers, 3});
  (void)setup_.F64("xyz_to_display", {papers, 3, 3});
  const auto reversals = static_cast<std::int64_t>(reversals_.size());
  (void)setup_.F64("reversal_layer_gain", {reversals, lights, 3});
  (void)setup_.F64("reversal_xyz_to_display", {reversals, 3, 3});
  (void)setup_.F64("negative_scan_aim_density", {negatives, lights, 3});
  (void)setup_.F64("negative_scan_light_axis", {negatives, lights, 3});
  for (const auto& negative : negatives_) {
    (void)stock_.F64(ArrayName("negative", negative.id, "scan_base_density"), {3});
  }
  for (const auto& light : negative_scan_constants_.axis_lights) {
    (void)LightSpectrum(light);
  }
  for (const auto& light : negative_balance_lights_) {
    (void)LightSpectrum(light);
  }
  for (const auto& medium : negative_scan_constants_.media) {
    for (std::size_t k = 0; k < medium.pool.size(); ++k) {
      (void)ScanMedium(medium.pool[k]);
      (void)LightIndex(medium.pool_scene_lights[k]);
    }
  }
  const auto spectrum_count = static_cast<std::int32_t>(scene_spectra_.size());
  for (const auto spectra : {setup_.I32("scene_spectrum_index", {negatives, lights}),
                             setup_.I32("reversal_scene_spectrum_index", {reversals, lights})}) {
    if (std::ranges::any_of(spectra, [spectrum_count](std::int32_t index) {
          return index < 0 || index >= spectrum_count;
        })) {
      throw std::runtime_error("Film print resource: a scene spectrum index is outside its list");
    }
  }
}

auto FilmStockBundle::Negative(std::string_view id) const -> FilmNegativeView {
  (void)IndexOf(negatives_, id, "negative");
  auto negative           = ReadFilmRecord("negative", id);
  negative.negative_grade = stock_.F64(ArrayName("negative", id, "negative_grade"), {})[0] != 0.0;
  return negative;
}

auto FilmStockBundle::IsReversal(std::string_view id) const -> bool {
  return std::ranges::binary_search(
      reversals_, id, {}, [](const FilmStockName& name) { return std::string_view(name.id); });
}

auto FilmStockBundle::Reversal(std::string_view id) const -> FilmReversalView {
  (void)IndexOf(reversals_, id, "reversal film");
  return {
      .record          = ReadFilmRecord("reversal", id),
      .dye_max         = stock_.F64(ArrayName("reversal", id, "dye_max"), {3}),
      .clear_luminance = stock_.F64(ArrayName("reversal", id, "clear_luminance"), {})[0],
  };
}

auto FilmStockBundle::Film(std::string_view id) const -> FilmNegativeView {
  return IsReversal(id) ? Reversal(id).record : Negative(id);
}

auto FilmStockBundle::ReversalSelection(std::string_view reversal,
                                        std::string_view scene_light) const -> FilmReversalSetup {
  return ReversalSelection(reversal, SingleLight(scene_light));
}

auto FilmStockBundle::ReversalSelection(std::string_view         reversal,
                                        const FilmSceneLightMix& scene_light) const
    -> FilmReversalSetup {
  const auto r         = IndexOf(reversals_, reversal, "reversal film");
  const auto a         = LightIndex(scene_light.first);
  const auto b         = LightIndex(scene_light.second);
  const auto reversals = static_cast<std::int64_t>(reversals_.size());
  const auto lights    = static_cast<std::int64_t>(scene_lights_.size());
  const auto gain      = setup_.F64("reversal_layer_gain", {reversals, lights, 3});
  const auto display   = setup_.F64("reversal_xyz_to_display", {reversals, 3, 3});
  const auto spectra   = setup_.I32("reversal_scene_spectrum_index", {reversals, lights});
  return {
      .layer_gain     = MixValues<3>(gain, r, lights, a, b, scene_light.share),
      .xyz_to_display = CopyValues<9>(display, r * 9),
      .scene_spectrum = MixSpectrum(spectra, r, scene_light),
  };
}

auto FilmStockBundle::ReadFilmRecord(std::string_view group, std::string_view id) const
    -> FilmNegativeView {
  const auto         field = [&](std::string_view name) { return ArrayName(group, id, name); };
  FilmInhibitionView inhibition;
  if (stock_.Contains(field("inhibition_free"))) {
    const auto shape = stock_.Shape(field("inhibition_free"));
    if (shape.size() != 2 || shape[0] != 3 || shape[1] < 2) {
      throw std::runtime_error("Film print resource: the inhibition curves of '" + std::string(id) +
                               "' do not have the shape (3, n)");
    }
    inhibition = {
        .k             = stock_.F64(field("inhibition_k"), {3, 3}),
        .image_k       = stock_.F64(field("inhibition_image_k"), {3, 3}),
        .free_values   = stock_.F64(field("inhibition_free"), {3, shape[1]}),
        .lengths       = stock_.I64(field("inhibition_lengths"), {3}),
        .log_h_start   = stock_.F64(field("inhibition_start"), {3}),
        .stored_length = shape[1],
        .log_h_step    = stock_.F64(field("inhibition_step"), {})[0],
    };
    RequireCurveLengths(inhibition.lengths, inhibition.stored_length, id);
  }
  return {
      .base               = stock_.F64(field("base"), {wavelengths_}),
      .dyes               = stock_.F64(field("dyes"), {3, wavelengths_}),
      .exposure_weights   = stock_.F64(field("exposure_weights"), {3, wavelengths_}),
      .density_grid_start = stock_.F64(field("density_grid_start"), {3}),
      .density_grid_stop  = stock_.F64(field("density_grid_stop"), {3}),
      .criterion_log_h    = stock_.F64(field("criterion_log_h"), {3}),
      .layer_spread_um    = stock_.F64(field("layer_spread_um"), {3}),
      .curves             = ReadCurves(stock_, field, id),
      .inhibition         = inhibition,
  };
}

auto FilmStockBundle::PrintMaterial(std::string_view id) const -> FilmPaperView {
  (void)IndexOf(papers_, id, "print material");
  const auto field = [&](std::string_view name) { return ArrayName("paper", id, name); };
  return {
      .base              = stock_.F64(field("base"), {wavelengths_}),
      .dyes              = stock_.F64(field("dyes"), {3, wavelengths_}),
      .printing_weights  = stock_.F64(field("printing_weights"), {3, wavelengths_}),
      .status_a_weights  = stock_.F64(field("status_a_weights"), {3, wavelengths_}),
      .viewing_weights   = stock_.F64(field("viewing_weights"), {3, wavelengths_}),
      .white_xyz         = stock_.F64(field("white_xyz"), {3}),
      .status_axis_start = stock_.F64(field("status_axis_start"), {3}),
      .status_axis_range = stock_.F64(field("status_axis_range"), {3}),
      .curves            = ReadCurves(stock_, field, id),
      .xyz_scale         = stock_.F64(field("xyz_scale"), {})[0],
      .clear_luminance   = stock_.F64(field("clear_luminance"), {})[0],
      .surround_exponent = stock_.F64(field("surround_exponent"), {})[0],
      .cinema_look       = stock_.F64(field("cinema_look"), {})[0] != 0.0,
  };
}

auto FilmStockBundle::Selection(std::string_view negative, std::string_view print_material,
                                std::string_view scene_light) const -> FilmSelectionSetup {
  return Selection(negative, print_material, SingleLight(scene_light));
}

auto FilmStockBundle::Selection(std::string_view negative, std::string_view print_material,
                                const FilmSceneLightMix& scene_light) const -> FilmSelectionSetup {
  const auto n         = IndexOf(negatives_, negative, "negative");
  const auto p         = IndexOf(papers_, print_material, "print material");
  const auto a         = LightIndex(scene_light.first);
  const auto b         = LightIndex(scene_light.second);
  const auto share     = scene_light.share;
  const auto negatives = static_cast<std::int64_t>(negatives_.size());
  const auto papers    = static_cast<std::int64_t>(papers_.size());
  const auto lights    = static_cast<std::int64_t>(scene_lights_.size());
  const auto balance   = setup_.F64("printer_balance_log10", {negatives, papers, lights, 3});
  const auto gain      = setup_.F64("layer_gain", {negatives, lights, 3});
  const auto printing  = setup_.F64("printing_gain", {negatives, papers, 3});
  const auto aim       = setup_.F64("aim_log_h", {papers, 3});
  const auto display   = setup_.F64("xyz_to_display", {papers, 3, 3});
  const auto spectra   = setup_.I32("scene_spectrum_index", {negatives, lights});
  const auto grey      = setup_.F64("negative_grade_grey", {negatives, lights, 3});
  // The lab check 'Grid blend, print' blends the printer balance of the two lights, in log10.
  return {
      .printer_balance_log10 = MixValues<3>(balance, n * papers + p, lights, a, b, share),
      .negative_grade_grey   = MixValues<3>(grey, n, lights, a, b, share),
      .layer_gain            = MixValues<3>(gain, n, lights, a, b, share),
      .printing_gain         = CopyValues<3>(printing, (n * papers + p) * 3),
      .aim_log_h             = CopyValues<3>(aim, p * 3),
      .xyz_to_display        = CopyValues<9>(display, p * 9),
      .scene_spectrum        = MixSpectrum(spectra, n, scene_light),
  };
}

auto FilmStockBundle::ScanMedium(std::string_view id) const -> const FilmScanMedium& {
  const auto  n      = static_cast<std::size_t>(IndexOf(negatives_, id, "negative"));
  const auto& media  = negative_scan_constants_.media;
  const auto  medium = std::ranges::find(media, negative_media_[n], &FilmScanMedium::id);
  if (medium == media.end()) {
    throw std::runtime_error("Film print: the negative '" + std::string(id) +
                             "' has no medium of Negative Scan");
  }
  return *medium;
}

auto FilmStockBundle::ScanBalanceLight(std::string_view id) const -> const std::string& {
  return negative_balance_lights_[static_cast<std::size_t>(IndexOf(negatives_, id, "negative"))];
}

auto FilmStockBundle::LightSpectrum(std::string_view name) const -> std::span<const double> {
  const auto key = ArrayName("light", name, "spectrum");
  if (!stock_.Contains(key)) {
    throw std::runtime_error("Film print resource: the bundle has no spectrum of the light '" +
                             std::string(name) + "'");
  }
  return stock_.F64(key, {wavelengths_});
}

auto FilmStockBundle::LayerGain(std::string_view negative, std::string_view scene_light) const
    -> std::array<double, 3> {
  const auto n         = IndexOf(negatives_, negative, "negative");
  const auto l         = LightIndex(scene_light);
  const auto negatives = static_cast<std::int64_t>(negatives_.size());
  const auto lights    = static_cast<std::int64_t>(scene_lights_.size());
  return CopyValues<3>(setup_.F64("layer_gain", {negatives, lights, 3}), (n * lights + l) * 3);
}

auto FilmStockBundle::NegativeScanSelection(std::string_view negative,
                                            std::string_view scene_light) const
    -> FilmNegativeScanSetup {
  return NegativeScanSelection(negative, SingleLight(scene_light));
}

auto FilmStockBundle::NegativeScanSelection(std::string_view         negative,
                                            const FilmSceneLightMix& scene_light) const
    -> FilmNegativeScanSetup {
  const auto  n         = IndexOf(negatives_, negative, "negative");
  const auto  a         = LightIndex(scene_light.first);
  const auto  b         = LightIndex(scene_light.second);
  const auto  share     = scene_light.share;
  const auto  negatives = static_cast<std::int64_t>(negatives_.size());
  const auto  lights    = static_cast<std::int64_t>(scene_lights_.size());
  const auto  gain      = setup_.F64("layer_gain", {negatives, lights, 3});
  const auto  aim       = setup_.F64("negative_scan_aim_density", {negatives, lights, 3});
  const auto  axis      = setup_.F64("negative_scan_light_axis", {negatives, lights, 3});
  const auto  spectra   = setup_.I32("scene_spectrum_index", {negatives, lights});
  const auto& medium    = ScanMedium(negative);
  // The lab form of a blend: the blended spectrum and layer gain, and the blend of the stored
  // values of the two lights.
  return {
      .medium         = medium.id,
      .balance_light  = negative_balance_lights_[static_cast<std::size_t>(n)],
      .layer_gain     = MixValues<3>(gain, n, lights, a, b, share),
      .base_density   = CopyValues<3>(
          stock_.F64(ArrayName("negative", negative, "scan_base_density"), {3}), 0),
      .aim_density    = MixValues<3>(aim, n, lights, a, b, share),
      .light_axis     = MixValues<3>(axis, n, lights, a, b, share),
      .gamma          = CopyValues<3>(medium.gamma, 0),
      .scene_spectrum = MixSpectrum(spectra, n, scene_light),
  };
}

}  // namespace alcedo
