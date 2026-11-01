//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/film/film_stock_bundle.hpp"

#include <algorithm>
#include <cstddef>
#include <json.hpp>
#include <stdexcept>
#include <utility>

#include "utils/hash/sha256.hpp"

namespace alcedo {
namespace {

constexpr std::string_view kManifestFileName = "manifest.json";
constexpr const char*      kReflectanceName  = "reflectance_over_uv_light";

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

}  // namespace

auto FilmStockBundle::Open(const std::filesystem::path& directory)
    -> std::shared_ptr<const FilmStockBundle> {
  const auto manifest = ReadManifest(directory / kManifestFileName);
  auto       stock    = ReadResource(directory, manifest, "stock_bundle");
  auto       setup    = ReadResource(directory, manifest, "print_setup");
  auto       table    = ReadResource(directory, manifest, "reflectance_table");
  const auto digests  = stock.sha256 + setup.sha256 + table.sha256;
  return std::shared_ptr<const FilmStockBundle>(
      new FilmStockBundle(std::move(stock.file), std::move(setup.file), std::move(table.file),
                          ComputeSha256Hex(std::as_bytes(std::span(digests)))));
}

FilmStockBundle::FilmStockBundle(FilmArrayFile stock, FilmArrayFile setup,
                                 FilmArrayFile reflectance, std::string content_digest)
    : stock_(std::move(stock)),
      setup_(std::move(setup)),
      reflectance_(std::move(reflectance)),
      content_digest_(std::move(content_digest)) {
  ReadLists();
  ReadCommonValues();
  CheckShapes();
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
    negatives_     = read_names("negatives");
    papers_        = read_names("print_materials");
    scene_lights_  = stock_.Metadata().at("scene_lights").get<std::vector<std::string>>();
    scene_spectra_ = stock_.Metadata().at("scene_spectra").get<std::vector<std::string>>();
  } catch (const nlohmann::json::exception& error) {
    throw std::runtime_error(std::string("Film print resource: the stock lists are not valid: ") +
                             error.what());
  }
  if (negatives_.empty() || papers_.empty() || scene_lights_.empty()) {
    throw std::runtime_error("Film print resource: a stock list is empty");
  }
}

void FilmStockBundle::ReadCommonValues() {
  const auto table = reflectance_.Shape(kReflectanceName);
  if (table.size() != 3 || table[0] != table[1]) {
    throw std::runtime_error(
        "Film print resource: the reflectance table does not have the shape (nodes, nodes, "
        "wavelengths)");
  }
  uv_nodes_       = table[0];
  wavelengths_    = table[2];
  print_contrast_ = stock_.F64("common/print_contrast", {})[0];
  print_flash_    = stock_.F64("common/print_flash", {})[0];
  viewing_flare_  = stock_.F64("common/viewing_flare", {})[0];
}

void FilmStockBundle::CheckShapes() const {
  for (const auto& negative : negatives_) {
    (void)Negative(negative.id);
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
  const auto spectra        = setup_.I32("scene_spectrum_index", {negatives, lights});
  const auto spectrum_count = static_cast<std::int32_t>(scene_spectra_.size());
  if (std::ranges::any_of(spectra, [spectrum_count](std::int32_t index) {
        return index < 0 || index >= spectrum_count;
      })) {
    throw std::runtime_error("Film print resource: a scene spectrum index is outside its list");
  }
}

auto FilmStockBundle::Negative(std::string_view id) const -> FilmNegativeView {
  (void)IndexOf(negatives_, id, "negative");
  const auto field = [&](std::string_view name) { return ArrayName("negative", id, name); };
  return {
      .base               = stock_.F64(field("base"), {wavelengths_}),
      .dyes               = stock_.F64(field("dyes"), {3, wavelengths_}),
      .exposure_weights   = stock_.F64(field("exposure_weights"), {3, wavelengths_}),
      .density_grid_start = stock_.F64(field("density_grid_start"), {3}),
      .density_grid_stop  = stock_.F64(field("density_grid_stop"), {3}),
  };
}

auto FilmStockBundle::PrintMaterial(std::string_view id) const -> FilmPaperView {
  (void)IndexOf(papers_, id, "print material");
  const auto field       = [&](std::string_view name) { return ArrayName("paper", id, name); };
  const auto curve_shape = stock_.Shape(field("curve_density"));
  if (curve_shape.size() != 2) {
    throw std::runtime_error("Film print resource: the curves of '" + std::string(id) +
                             "' do not have two axes");
  }
  const auto stored_length = curve_shape[1];
  const auto lengths       = stock_.I64(field("curve_lengths"), {3});
  if (std::ranges::any_of(lengths, [stored_length](std::int64_t length) {
        return length < 2 || length > stored_length;
      })) {
    throw std::runtime_error("Film print resource: the curves of '" + std::string(id) +
                             "' have an invalid length");
  }
  return {
      .base              = stock_.F64(field("base"), {wavelengths_}),
      .dyes              = stock_.F64(field("dyes"), {3, wavelengths_}),
      .printing_weights  = stock_.F64(field("printing_weights"), {3, wavelengths_}),
      .status_a_weights  = stock_.F64(field("status_a_weights"), {3, wavelengths_}),
      .viewing_weights   = stock_.F64(field("viewing_weights"), {3, wavelengths_}),
      .white_xyz         = stock_.F64(field("white_xyz"), {3}),
      .status_axis_start = stock_.F64(field("status_axis_start"), {3}),
      .status_axis_range = stock_.F64(field("status_axis_range"), {3}),
      .curves =
          {
              .density       = stock_.F64(field("curve_density"), {3, stored_length}),
              .lengths       = lengths,
              .log_h_start   = stock_.F64(field("curve_log_h_start"), {3}),
              .stored_length = stored_length,
              .log_h_step    = stock_.F64(field("curve_log_h_step"), {})[0],
          },
      .xyz_scale = stock_.F64(field("xyz_scale"), {})[0],
  };
}

auto FilmStockBundle::Selection(std::string_view negative, std::string_view print_material,
                                std::string_view scene_light) const -> FilmSelectionSetup {
  const auto n     = IndexOf(negatives_, negative, "negative");
  const auto p     = IndexOf(papers_, print_material, "print material");
  const auto light = std::find(scene_lights_.begin(), scene_lights_.end(), scene_light);
  if (light == scene_lights_.end()) {
    throw std::runtime_error("Film print: unknown scene light '" + std::string(scene_light) + "'");
  }
  const auto  l         = light - scene_lights_.begin();
  const auto  negatives = static_cast<std::int64_t>(negatives_.size());
  const auto  papers    = static_cast<std::int64_t>(papers_.size());
  const auto  lights    = static_cast<std::int64_t>(scene_lights_.size());
  const auto  balance   = setup_.F64("printer_balance_log10", {negatives, papers, lights, 3});
  const auto  gain      = setup_.F64("layer_gain", {negatives, lights, 3});
  const auto  printing  = setup_.F64("printing_gain", {negatives, papers, 3});
  const auto  aim       = setup_.F64("aim_log_h", {papers, 3});
  const auto  display   = setup_.F64("xyz_to_display", {papers, 3, 3});
  const auto  spectra   = setup_.I32("scene_spectrum_index", {negatives, lights});
  const auto& spectrum  = scene_spectra_[static_cast<std::size_t>(spectra[n * lights + l])];
  return {
      .printer_balance_log10 = CopyValues<3>(balance, ((n * papers + p) * lights + l) * 3),
      .layer_gain            = CopyValues<3>(gain, (n * lights + l) * 3),
      .printing_gain         = CopyValues<3>(printing, (n * papers + p) * 3),
      .aim_log_h             = CopyValues<3>(aim, p * 3),
      .xyz_to_display        = CopyValues<9>(display, p * 9),
      .scene_spectrum        = stock_.F64(ArrayName("light", spectrum, "spectrum"), {wavelengths_}),
  };
}

auto FilmStockBundle::ReflectanceTable() const -> std::span<const float> {
  return reflectance_.F32(kReflectanceName, {uv_nodes_, uv_nodes_, wavelengths_});
}

}  // namespace alcedo
