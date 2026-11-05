//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/film/film_print_tables.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <json.hpp>
#include <map>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "edit/graph/film_print_model.hpp"
#include "edit/runtime/film/film_array_file.hpp"
#include "edit/runtime/film/film_negative_scan.hpp"
#include "edit/runtime/film/film_print_kernel_tables.hpp"
#include "edit/runtime/film/film_print_math.h"
#include "edit/runtime/film/film_stock_bundle.hpp"
#include "edit/runtime/film/film_tables.hpp"
#include "film_print_reference.hpp"
#include "utils/hash/sha256.hpp"

namespace alcedo {
namespace {

namespace fs                        = std::filesystem;

constexpr std::int64_t kWavelengths = 6;
constexpr std::int64_t kUvNodes     = 4;

struct ArrayValues {
  std::string               type;
  std::vector<std::int64_t> shape;
  std::vector<std::byte>    bytes;
};

template <class T>
auto ToBytes(const std::vector<T>& values) -> std::vector<std::byte> {
  const auto bytes = std::as_bytes(std::span(values));
  return {bytes.begin(), bytes.end()};
}

auto F64Array(std::vector<std::int64_t> shape, std::vector<double> values) -> ArrayValues {
  return {"f8", std::move(shape), ToBytes(values)};
}

/// The array file form of `build_product_resources.py`.
void WriteArrayFile(const fs::path& path, const std::map<std::string, ArrayValues>& arrays,
                    const nlohmann::json& metadata,
                    int                   schema_version = kFilmResourceSchemaVersion) {
  nlohmann::json         index = nlohmann::json::object();
  std::vector<std::byte> data;
  for (const auto& [name, array] : arrays) {
    index[name] = {{"type", array.type}, {"shape", array.shape}, {"offset", data.size()}};
    data.insert(data.end(), array.bytes.begin(), array.bytes.end());
    data.resize((data.size() + 7) / 8 * 8);
  }
  auto header =
      nlohmann::json{{"schema_version", schema_version}, {"metadata", metadata}, {"arrays", index}}
          .dump();
  header.resize((header.size() + 7) / 8 * 8, ' ');
  std::ofstream       stream(path, std::ios::binary);
  const std::uint64_t length = header.size();
  stream.write("ALCDFILM", 8);
  stream.write(reinterpret_cast<const char*>(&length), sizeof(length));
  stream.write(header.data(), static_cast<std::streamsize>(header.size()));
  stream.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
  ASSERT_TRUE(stream.good()) << path;
}

auto ReadBytes(const fs::path& path) -> std::vector<std::byte> {
  return ReadFilmResourceBytes(path);
}

void WriteBytes(const fs::path& path, std::span<const std::byte> bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(stream.good()) << path;
}

/// The manifest of the three resource files in @p directory.
void WriteManifest(const fs::path& directory, int schema_version = kFilmResourceSchemaVersion) {
  nlohmann::json                           resources = nlohmann::json::object();
  const std::map<std::string, std::string> files{
      {"stock_bundle", "film_stock_bundle.bin"},
      {"print_setup", "film_print_setup.bin"},
      {"reflectance_table", "film_reflectance_table.bin"}};
  for (const auto& [key, file] : files) {
    const auto bytes = ReadBytes(directory / file);
    resources[key] = {{"file", file}, {"bytes", bytes.size()}, {"sha256", ComputeSha256Hex(bytes)}};
  }
  const auto text =
      nlohmann::json{{"schema_version", schema_version}, {"resources", resources}}.dump();
  WriteBytes(directory / "manifest.json", std::as_bytes(std::span(text)));
}

/// The compact reflectance file: rows of the kept nodes, with each byte of a float32 in its plane.
void WriteReflectance(const fs::path& path, const std::vector<std::int32_t>& nodes,
                      std::int32_t first_wavelength, std::int32_t wavelength_count) {
  std::vector<float> rows;
  for (std::size_t value = 0; value < nodes.size() * wavelength_count; ++value) {
    rows.push_back(0.25f + 0.01f * static_cast<float>(value));
  }
  const auto             bytes = std::as_bytes(std::span(rows));
  std::vector<std::byte> planes;
  for (std::size_t plane = 0; plane < sizeof(float); ++plane) {
    for (std::size_t value = 0; value < rows.size(); ++value) {
      planes.push_back(bytes[value * sizeof(float) + plane]);
    }
  }
  const auto values = static_cast<std::int64_t>(rows.size());
  WriteArrayFile(
      path,
      {{"byte_planes", ArrayValues{"u1", {4, values}, std::move(planes)}},
       {"node_index", ArrayValues{"i4", {static_cast<std::int64_t>(nodes.size())}, ToBytes(nodes)}},
       {"uv_nodes", ArrayValues{"i4", {}, ToBytes<std::int32_t>({kUvNodes})}},
       {"first_wavelength", ArrayValues{"i4", {}, ToBytes<std::int32_t>({first_wavelength})}},
       {"wavelength_count", ArrayValues{"i4", {}, ToBytes<std::int32_t>({wavelength_count})}}},
      nlohmann::json::object());
}

/// All nodes of the test table except the first and the last.
auto KeptNodes() -> std::vector<std::int32_t> {
  std::vector<std::int32_t> nodes;
  for (std::int32_t node = 1; node + 1 < kUvNodes * kUvNodes; ++node) {
    nodes.push_back(node);
  }
  return nodes;
}

/// Three rows of @p wavelengths values: each row has its own smooth bump.
auto BandRows(double scale, double offset) -> std::vector<double> {
  std::vector<double> rows;
  for (int row = 0; row < 3; ++row) {
    for (std::int64_t w = 0; w < kWavelengths; ++w) {
      const auto distance = static_cast<double>(w) - (static_cast<double>(row) * 2.5);
      rows.push_back(scale * std::exp(-0.5 * distance * distance) + offset);
    }
  }
  return rows;
}

auto Flat(double value) -> std::vector<double> {
  return std::vector<double>(static_cast<std::size_t>(kWavelengths), value);
}

/// The wavelengths of the synthetic bundle: each one is inside a band of the default scanner.
const std::vector<double> kWavelengthsNm{400.0, 460.0, 520.0, 580.0, 640.0, 700.0};

/// Small resources with two negatives, two papers, and two scene lights. The values have no
/// physical meaning. Each one is distinct, so that a wrong index gives a wrong table.
/// @param negative_scan_id The identifier of Negative Scan in the metadata.
/// @param first_gamma The first gamma of the medium 'cine'.
/// @param neg_b_medium The medium of neg_b.
void WriteSyntheticResources(const fs::path& directory, double surround_exponent = 1.0,
                             const std::string& negative_scan_id = "negative_scan",
                             double             first_gamma      = 0.48,
                             const std::string& neg_b_medium     = "photo") {
  fs::create_directories(directory);
  std::map<std::string, ArrayValues> stock;
  stock["common/wavelengths_nm"] = F64Array({kWavelengths}, kWavelengthsNm);
  stock["common/print_contrast"] = F64Array({}, {0.85});
  stock["common/print_flash"]    = F64Array({}, {0.02});
  stock["common/viewing_flare"]  = F64Array({}, {0.003});
  stock["common/ap1_to_xyz"]     = F64Array({3, 3}, {0.7, 0.2, 0.1, 0.3, 0.6, 0.1, 0.0, 0.1, 0.9});
  stock["common/locus_normals"]  = F64Array({2, 2}, {1.0, 0.0, 0.0, 1.0});
  stock["common/locus_room"]     = F64Array({2}, {4.0, 5.0});
  stock["common/locus_white"]    = F64Array({2}, {0.95, 1.08});
  stock["common/locus_inset"]    = F64Array({}, {0.001});
  stock["common/uv_max"]         = F64Array({2}, {0.66, 0.62});
  stock["common/gamut_threshold"] = F64Array({3}, {0.815, 0.803, 0.88});
  stock["common/gamut_limit"]     = F64Array({3}, {1.4, 1.3, 1.312});
  stock["common/gamut_power"]     = F64Array({}, {1.2});
  stock["common/halation_strength"]   = F64Array({3}, {0.03, 0.008, 0.0});
  stock["common/spread_gaussians"]    = F64Array({2, 2}, {0.25, 0.4, 0.75, 1.4});
  stock["common/frame_mm"]            = F64Array({}, {36.0});
  stock["common/halation_um"]         = F64Array({}, {120.0});
  stock["common/inhibition_um"]       = F64Array({}, {35.0});
  stock["common/enlarger_flare_size"] = F64Array({}, {0.1});
  // The default scanner of Negative Scan, as the lab (rule 'ideal_scanner_bands').
  stock["common/negative_scan_centres_nm"]      = F64Array({3}, {650.0, 545.0, 450.0});
  stock["common/negative_scan_width_nm"]        = F64Array({}, {40.0});
  stock["common/negative_scan_band_cut"]        = F64Array({}, {1e-4});
  stock["common/negative_scan_lamp_kelvin"]     = F64Array({}, {3200.0});
  stock["common/negative_scan_planck_c2_nm_k"]  = F64Array({}, {1.4388e7});
  stock["scan_medium/cine/gamma"]               = F64Array({3}, {first_gamma, 0.55, 0.53});
  stock["scan_medium/photo/gamma"]              = F64Array({3}, {0.56, 0.57, 0.62});
  stock["light/flat/spectrum"]   = F64Array({kWavelengths}, Flat(1.0));
  stock["light/ramp/spectrum"]   = F64Array({kWavelengths}, {0.5, 0.75, 1.0, 1.25, 1.5, 1.75});
  for (const auto& stock_entry : std::map<std::string, double>{{"neg_a", 1.0}, {"neg_b", 1.2}}) {
    const auto& id        = stock_entry.first;
    const auto  scale     = stock_entry.second;
    const auto  name      = [&id](const char* field) { return "negative/" + id + "/" + field; };
    stock[name("base")]   = F64Array({kWavelengths}, Flat(0.1 * scale));
    stock[name("dyes")]   = F64Array({3, kWavelengths}, BandRows(scale, 0.01));
    // The last wavelength has no weight, so the reflectance table does not hold it.
    auto exposure_weights = BandRows(0.3 * scale, 0.0);
    for (std::size_t row = 1; row <= 3; ++row) {
      exposure_weights[row * kWavelengths - 1] = 0.0;
    }
    stock[name("exposure_weights")]   = F64Array({3, kWavelengths}, exposure_weights);
    stock[name("density_grid_start")] = F64Array({3}, {0.0, 0.01, 0.02});
    stock[name("density_grid_stop")]  = F64Array({3}, {2.0 * scale, 2.5 * scale, 3.0 * scale});
    stock[name("criterion_log_h")]    = F64Array({3}, {-1.0, -1.1, -1.2});
    stock[name("layer_spread_um")]    = F64Array({3}, {5.0, 3.0, 3.0});
    stock[name("negative_grade")]     = F64Array({}, {0.0});
    stock[name("curve_density")]      = F64Array(
        {3, 4}, {0.1, 0.4 * scale, 0.9, 1.5, 0.1, 0.5, 1.0, 1.6, 0.1, 0.3, 0.8, 0.8});
    stock[name("curve_lengths")]     = ArrayValues{"i8", {3}, ToBytes<std::int64_t>({4, 4, 3})};
    stock[name("curve_log_h_start")] = F64Array({3}, {-3.0, -2.5, -2.0});
    stock[name("curve_log_h_step")]  = F64Array({}, {0.5});
    stock[name("scan_base_density")]  = F64Array({3}, {0.2 * scale, 0.3 * scale, 0.4 * scale});
  }
  // neg_b has no inhibition, as a stock with no modulation transfer data.
  stock["negative/neg_a/inhibition_k"] =
      F64Array({3, 3}, {0.1, 0.02, 0.03, 0.04, 0.2, 0.05, 0.06, 0.07, 0.3});
  stock["negative/neg_a/inhibition_image_k"] =
      F64Array({3, 3}, {0.05, 0.02, 0.03, 0.0, 0.0, 0.0, 0.06, 0.07, 0.3});
  stock["negative/neg_a/inhibition_free"] =
      F64Array({3, 3}, {0.1, 0.6, 1.1, 0.2, 0.7, 1.2, 0.3, 0.8, 0.8});
  stock["negative/neg_a/inhibition_lengths"] =
      ArrayValues{"i8", {3}, ToBytes<std::int64_t>({3, 3, 2})};
  stock["negative/neg_a/inhibition_start"] = F64Array({3}, {-3.2, -2.7, -2.2});
  stock["negative/neg_a/inhibition_step"]  = F64Array({}, {0.75});
  for (const auto& stock_entry :
       std::map<std::string, double>{{"paper_a", 1.0}, {"paper_b", 0.8}}) {
    const auto& id      = stock_entry.first;
    const auto  scale   = stock_entry.second;
    const auto  name    = [&id](const char* field) { return "paper/" + id + "/" + field; };
    stock[name("base")] = F64Array({kWavelengths}, Flat(0.05));
    stock[name("dyes")] = F64Array({3, kWavelengths}, BandRows(scale, 0.01));
    stock[name("printing_weights")]  = F64Array({3, kWavelengths}, BandRows(scale, 0.0));
    stock[name("status_a_weights")]  = F64Array({3, kWavelengths}, BandRows(1.0, 0.0));
    stock[name("viewing_weights")]   = F64Array({3, kWavelengths}, BandRows(scale, 0.02));
    stock[name("white_xyz")]         = F64Array({3}, {96.0, 100.0, 82.0 * scale});
    stock[name("status_axis_start")] = F64Array({3}, {0.1, 0.1, 0.1});
    stock[name("status_axis_range")] = F64Array({3}, {2.0, 2.0, 2.0});
    stock[name("xyz_scale")]         = F64Array({}, {10.0 * scale});
    stock[name("clear_luminance")]   = F64Array({}, {100.0});
    stock[name("surround_exponent")] = F64Array({}, {surround_exponent});
    stock[name("cinema_look")]       = F64Array({}, {0.0});
    stock[name("curve_density")]     = F64Array(
        {3, 5}, {0.1, 0.5, 1.0, 1.6, 2.1, 0.1, 0.6, 1.1, 1.7, 2.1, 0.1, 0.4, 0.9, 1.5, 1.5});
    stock[name("curve_lengths")]     = ArrayValues{"i8", {3}, ToBytes<std::int64_t>({5, 5, 4})};
    stock[name("curve_log_h_start")] = F64Array({3}, {-3.0, -3.0, -3.0});
    stock[name("curve_log_h_step")]  = F64Array({}, {1.5});
  }
  // One colour reversal film with no inhibition, and the values of the light table.
  {
    const auto name     = [](const char* field) { return std::string("reversal/rev_a/") + field; };
    stock[name("base")] = F64Array({kWavelengths}, Flat(0.08));
    stock[name("dyes")] = F64Array({3, kWavelengths}, BandRows(0.9, 0.02));
    auto exposure_weights = BandRows(0.25, 0.0);
    for (std::size_t row = 1; row <= 3; ++row) {
      exposure_weights[row * kWavelengths - 1] = 0.0;
    }
    stock[name("exposure_weights")]   = F64Array({3, kWavelengths}, exposure_weights);
    stock[name("density_grid_start")] = F64Array({3}, {0.0, 0.0, 0.0});
    stock[name("density_grid_stop")]  = F64Array({3}, {2.8, 3.0, 3.2});
    stock[name("dye_max")]            = F64Array({3}, {2.9, 3.1, 3.2});
    stock[name("clear_luminance")]    = F64Array({}, {85.0});
    stock[name("criterion_log_h")]    = F64Array({3}, {-1.5, -1.6, -1.7});
    stock[name("layer_spread_um")]    = F64Array({3}, {4.0, 4.0, 3.0});
    stock[name("curve_density")] =
        F64Array({3, 4}, {0.0, 0.7, 2.0, 2.8, 0.0, 0.8, 2.2, 3.0, 0.0, 0.9, 2.4, 3.2});
    stock[name("curve_lengths")]     = ArrayValues{"i8", {3}, ToBytes<std::int64_t>({4, 4, 4})};
    stock[name("curve_log_h_start")] = F64Array({3}, {-3.5, -3.4, -3.3});
    stock[name("curve_log_h_step")]  = F64Array({}, {0.6});
  }
  stock["light_table/viewing_weights"]   = F64Array({3, kWavelengths}, BandRows(0.6, 0.05));
  stock["light_table/display_luminance"] = F64Array({3}, {0.2126, 0.7152, 0.0722});
  stock["light_table/xyz_scale"]         = F64Array({}, {12.5});
  stock["light_table/surround_exponent"] = F64Array({}, {0.89});
  stock["light_table/viewing_cct_k"]     = F64Array({}, {5000.0});
  // neg_a reads the spectrum 'ramp' as its balance light, and neg_b the spectrum 'flat'. The light
  // axis goes from 'ramp' to 'flat'. The pool of 'photo' has both negatives.
  const nlohmann::json metadata{
      {"negatives",
       {{"neg_a", {{"name", "Negative A"}, {"medium", "cine"}, {"scan_balance_light", "ramp"}}},
        {"neg_b",
         {{"name", "Negative B"}, {"medium", neg_b_medium}, {"scan_balance_light", "flat"}}}}},
      {"print_materials", {{"paper_a", {{"name", "Paper A"}}}, {"paper_b", {{"name", "Paper B"}}}}},
      {"reversal_films", {{"rev_a", {{"name", "Reversal A"}}}}},
      {"reversal_list_order", {"rev_a"}},
      {"scene_lights", {"reference", "daylight"}},
      {"scene_spectra", {"flat", "ramp"}},
      {"negative_scan",
       {{"id", negative_scan_id},
        {"name", "Negative Scan"},
        {"light", "halogen"},
        {"lights", {"halogen", "equal_energy"}},
        {"axis_lights", {"ramp", "flat"}},
        {"media",
         {{"cine", {{"pool", {"neg_a"}}, {"pool_scene_lights", {"daylight"}}}},
          {"photo",
           {{"pool", {"neg_a", "neg_b"}}, {"pool_scene_lights", {"reference", "daylight"}}}}}}}},
      // A grid of two nodes. The lights are those of the list, so the arrays keep two lights.
      {"cct_grid",
       {{"kelvin", {3000, 6000}},
        {"lights", {"reference", "daylight"}},
        {"mired", {1e6 / 3000.0, 1e6 / 6000.0}}}}};
  WriteArrayFile(directory / "film_stock_bundle.bin", stock, metadata);

  std::vector<double> balance;
  for (int value = 0; value < 2 * 2 * 2 * 3; ++value) {
    balance.push_back(-1.0 + 0.01 * value);
  }
  std::map<std::string, ArrayValues> setup;
  setup["printer_balance_log10"] = F64Array({2, 2, 2, 3}, balance);
  setup["layer_gain"] =
      F64Array({2, 2, 3}, {1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9, 2.0, 2.1});
  setup["printing_gain"] =
      F64Array({2, 2, 3}, {1.0, 1.0, 1.0, 1.0, 0.9, 1.1, 1.0, 1.0, 1.0, 1.0, 0.95, 1.05});
  setup["aim_log_h"]            = F64Array({2, 3}, {-1.0, -1.1, -1.2, -0.9, -0.8, -0.7});
  setup["negative_grade_grey"]  = F64Array({2, 2, 3}, std::vector<double>(12, 0.0));
  setup["xyz_to_display"]       = F64Array({2, 3, 3}, std::vector<double>(18, 0.01));
  setup["scene_spectrum_index"] = ArrayValues{"i4", {2, 2}, ToBytes<std::int32_t>({0, 1, 1, 0})};
  setup["reversal_layer_gain"]  = F64Array({1, 2, 3}, {0.7, 0.8, 0.9, 1.7, 1.8, 1.9});
  setup["reversal_xyz_to_display"] =
      F64Array({1, 3, 3}, {3.2, -1.5, -0.5, -0.9, 1.9, 0.04, 0.05, -0.2, 1.06});
  // Under 'reference' the reversal film reads the spectrum 'ramp', as a film with a daylight
  // balance.
  setup["reversal_scene_spectrum_index"] = ArrayValues{"i4", {1, 2}, ToBytes<std::int32_t>({1, 0})};
  // Negative Scan, per negative and scene light.
  setup["negative_scan_aim_density"] =
      F64Array({2, 2, 3}, {0.9, 1.0, 1.1, 0.95, 1.05, 1.15, 0.8, 0.85, 0.9, 0.82, 0.87, 0.92});
  setup["negative_scan_light_axis"] = F64Array(
      {2, 2, 3}, {0.1, 0.0, -0.2, 0.11, 0.01, -0.21, 0.12, 0.02, -0.22, 0.13, 0.03, -0.23});
  WriteArrayFile(directory / "film_print_setup.bin", setup, nlohmann::json::object());

  WriteReflectance(directory / "film_reflectance_table.bin", KeptNodes(), 0, kWavelengths - 1);
  WriteManifest(directory);
}

/// The message of the exception of FilmStockBundle::Open, or empty when it does not throw.
auto OpenError(const fs::path& directory) -> std::string {
  try {
    (void)FilmStockBundle::Open(directory);
  } catch (const std::runtime_error& error) {
    return error.what();
  }
  return {};
}

class FilmPrintTablesTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    std::random_device random;
    root_ = fs::temp_directory_path() / ("alcedo_film_print_tables_" + std::to_string(random()));
    WriteSyntheticResources(root_ / "resources");
  }

  static void TearDownTestSuite() {
    std::error_code error;
    fs::remove_all(root_, error);
  }

  /// A copy of the synthetic resources that a test can change.
  [[nodiscard]] auto CopyOfResources(const std::string& name) const -> fs::path {
    const auto directory = root_ / name;
    fs::copy(root_ / "resources", directory, fs::copy_options::recursive);
    return directory;
  }

  static auto            Resources() -> fs::path { return root_ / "resources"; }

  static inline fs::path root_;
};

TEST_F(FilmPrintTablesTest, OpenReadsTheListsAndTheSelectionValues) {
  const auto bundle = FilmStockBundle::Open(Resources());
  ASSERT_EQ(bundle->Negatives().size(), 2u);
  EXPECT_EQ(bundle->Negatives()[1].id, "neg_b");
  EXPECT_EQ(bundle->Negatives()[1].display_name, "Negative B");
  ASSERT_EQ(bundle->PrintMaterials().size(), 2u);
  EXPECT_EQ(bundle->PrintMaterials()[0].id, "paper_a");
  ASSERT_EQ(bundle->SceneLights().size(), 2u);
  EXPECT_EQ(bundle->SceneLights()[1], "daylight");
  EXPECT_EQ(bundle->UvNodes(), kUvNodes);
  EXPECT_EQ(bundle->Wavelengths(), kWavelengths);
  EXPECT_DOUBLE_EQ(bundle->PrintContrast(), 0.85);

  const auto selection = bundle->Selection("neg_b", "paper_a", "daylight");
  // Index (1, 0, 1) of the (2, 2, 2, 3) balance array starts at value 15.
  EXPECT_DOUBLE_EQ(selection.printer_balance_log10[0], -1.0 + 0.01 * 15);
  EXPECT_DOUBLE_EQ(selection.layer_gain[0], 1.9);
  EXPECT_DOUBLE_EQ(selection.printing_gain[2], 1.0);
  EXPECT_DOUBLE_EQ(selection.aim_log_h[1], -1.1);
  // Index (1, 1) of scene_spectrum_index is spectrum 0, 'flat'.
  EXPECT_DOUBLE_EQ(selection.scene_spectrum[5], 1.0);
}

TEST_F(FilmPrintTablesTest, DamagedResourceThrows) {
  for (const std::string file :
       {"film_stock_bundle.bin", "film_print_setup.bin", "film_reflectance_table.bin"}) {
    SCOPED_TRACE(file);
    const auto directory = CopyOfResources("damaged_" + file);
    auto       bytes     = ReadBytes(directory / file);
    bytes[bytes.size() / 2] ^= std::byte{0x01};
    WriteBytes(directory / file, bytes);
    const auto error = OpenError(directory);
    EXPECT_NE(error.find(file), std::string::npos) << error;
    EXPECT_NE(error.find("SHA-256"), std::string::npos) << error;
  }
}

TEST_F(FilmPrintTablesTest, MissingFileThrowsWithItsName) {
  for (const std::string file : {"manifest.json", "film_print_setup.bin"}) {
    SCOPED_TRACE(file);
    const auto directory = CopyOfResources("missing_" + file);
    fs::remove(directory / file);
    const auto error = OpenError(directory);
    EXPECT_NE(error.find(file), std::string::npos) << error;
  }
}

TEST_F(FilmPrintTablesTest, ShortenedFileThrowsWithItsSize) {
  const auto directory = CopyOfResources("shortened");
  auto       bytes     = ReadBytes(directory / "film_print_setup.bin");
  bytes.pop_back();
  WriteBytes(directory / "film_print_setup.bin", bytes);
  const auto error = OpenError(directory);
  EXPECT_NE(error.find("film_print_setup.bin"), std::string::npos) << error;
  EXPECT_NE(error.find("bytes"), std::string::npos) << error;
}

TEST_F(FilmPrintTablesTest, OtherManifestSchemaVersionThrows) {
  const auto directory = CopyOfResources("manifest_schema");
  WriteManifest(directory, kFilmResourceSchemaVersion + 1);
  const auto error = OpenError(directory);
  EXPECT_NE(error.find("manifest.json"), std::string::npos) << error;
  EXPECT_NE(error.find("schema version"), std::string::npos) << error;
}

TEST_F(FilmPrintTablesTest, OtherArrayFileSchemaVersionThrows) {
  const auto directory = CopyOfResources("array_schema");
  WriteArrayFile(directory / "film_print_setup.bin", {}, nlohmann::json::object(),
                 kFilmResourceSchemaVersion + 1);
  WriteManifest(directory);
  const auto error = OpenError(directory);
  EXPECT_NE(error.find("film_print_setup.bin"), std::string::npos) << error;
  EXPECT_NE(error.find("schema version"), std::string::npos) << error;
}

TEST_F(FilmPrintTablesTest, ArrayOfTheWrongShapeThrowsWithItsName) {
  const auto directory = CopyOfResources("wrong_shape");
  WriteArrayFile(directory / "film_print_setup.bin",
                 {{"printer_balance_log10", F64Array({1, 3}, {0.0, 0.0, 0.0})}},
                 nlohmann::json::object());
  WriteManifest(directory);
  const auto error = OpenError(directory);
  EXPECT_NE(error.find("film_print_setup.bin"), std::string::npos) << error;
  EXPECT_NE(error.find("printer_balance_log10"), std::string::npos) << error;
}

TEST_F(FilmPrintTablesTest, ExposureWeightOutsideTheReflectanceBandThrows) {
  const auto directory = CopyOfResources("short_band");
  WriteReflectance(directory / "film_reflectance_table.bin", KeptNodes(), 0, kWavelengths - 2);
  WriteManifest(directory);
  const auto error = OpenError(directory);
  EXPECT_NE(error.find("neg_a"), std::string::npos) << error;
  EXPECT_NE(error.find("wavelength band"), std::string::npos) << error;
}

TEST_F(FilmPrintTablesTest, ReflectanceBandPastTheWavelengthListThrows) {
  const auto directory = CopyOfResources("band_past_end");
  WriteReflectance(directory / "film_reflectance_table.bin", KeptNodes(), 1, kWavelengths);
  WriteManifest(directory);
  const auto error = OpenError(directory);
  EXPECT_NE(error.find("film_reflectance_table.bin"), std::string::npos) << error;
}

TEST_F(FilmPrintTablesTest, ReflectanceNodesThatDoNotRiseThrow) {
  for (const auto& nodes :
       {std::vector<std::int32_t>{2, 1}, std::vector<std::int32_t>{3, 3},
        std::vector<std::int32_t>{1, kUvNodes * kUvNodes}, std::vector<std::int32_t>{-1, 1}}) {
    const auto directory =
        CopyOfResources("nodes_" + std::to_string(nodes[0]) + "_" + std::to_string(nodes[1]));
    WriteReflectance(directory / "film_reflectance_table.bin", nodes, 0, kWavelengths - 1);
    WriteManifest(directory);
    const auto error = OpenError(directory);
    EXPECT_NE(error.find("film_reflectance_table.bin"), std::string::npos) << error;
    EXPECT_NE(error.find("node list"), std::string::npos) << error;
  }
}

TEST_F(FilmPrintTablesTest, OpenRestoresTheReflectanceRowsFromTheirBytePlanes) {
  const auto bundle = FilmStockBundle::Open(Resources());
  EXPECT_EQ(bundle->Wavelengths(), kWavelengths);
  EXPECT_EQ(bundle->ReflectanceFirstWavelength(), 0);
  EXPECT_EQ(bundle->ReflectanceWavelengths(), kWavelengths - 1);
  const auto nodes = KeptNodes();
  ASSERT_EQ(bundle->ReflectanceNodes().size(), nodes.size());
  const auto rows = bundle->ReflectanceRows();
  ASSERT_EQ(rows.size(), nodes.size() * static_cast<std::size_t>(kWavelengths - 1));
  for (std::size_t value = 0; value < rows.size(); ++value) {
    EXPECT_EQ(rows[value], 0.25f + 0.01f * static_cast<float>(value));
  }
}

TEST_F(FilmPrintTablesTest, ArrayPastTheEndOfTheFileThrows) {
  const fs::path path = root_ / "past_the_end.bin";
  WriteArrayFile(path, {{"values", F64Array({2}, {1.0, 2.0})}}, nlohmann::json::object());
  auto bytes = ReadBytes(path);
  bytes.resize(bytes.size() - 8);
  EXPECT_THROW((void)FilmArrayFile::Parse(std::move(bytes), path), std::runtime_error);
}

TEST_F(FilmPrintTablesTest, UnknownSelectionThrowsWithTheIdentifier) {
  FilmPrintTables tables(FilmStockBundle::Open(Resources()));
  const auto message_of = [&tables](const char* negative, const char* paper, const char* light) {
    try {
      (void)tables.Resolve(negative, paper, light);
    } catch (const std::runtime_error& error) {
      return std::string(error.what());
    }
    return std::string{};
  };
  EXPECT_NE(message_of("no_negative", "paper_a", "reference").find("no_negative"),
            std::string::npos);
  EXPECT_NE(message_of("neg_a", "no_paper", "reference").find("no_paper"), std::string::npos);
  EXPECT_NE(message_of("neg_a", "paper_a", "no_light").find("no_light"), std::string::npos);
  EXPECT_EQ(tables.Stats().compose_builds, 0);
  EXPECT_EQ(tables.Stats().kept_bytes, 0u);
}

TEST_F(FilmPrintTablesTest, UvTableIsTheIntegralUnderTheSceneSpectrum) {
  const auto      bundle = FilmStockBundle::Open(Resources());
  FilmPrintTables tables(bundle);
  // neg_a under 'daylight' reads spectrum 1, 'ramp'.
  const auto      result = tables.Resolve("neg_a", "paper_a", "daylight");
  ASSERT_EQ(result->uv_nodes, kUvNodes);
  const auto stock = bundle->Negative("neg_a");
  const auto setup = bundle->Selection("neg_a", "paper_a", "daylight");
  const auto rows  = bundle->ReflectanceRows();
  const auto nodes = bundle->ReflectanceNodes();
  const auto band  = static_cast<std::size_t>(bundle->ReflectanceWavelengths());
  for (std::size_t row = 0; row < nodes.size(); ++row) {
    for (std::size_t layer = 0; layer < 3; ++layer) {
      double expected = 0.0;
      for (std::size_t w = 0; w < band; ++w) {
        expected += static_cast<double>(rows[row * band + w]) * setup.scene_spectrum[w] *
                    stock.exposure_weights[layer * kWavelengths + w];
      }
      const auto value = result->uv_exposure[static_cast<std::size_t>(nodes[row]) * 3 + layer];
      EXPECT_NEAR(value, expected, 1e-12 * expected);
      EXPECT_GT(value, 0.0);
    }
  }
  // The first and the last node are not in the file, and no read reaches them.
  const auto last = static_cast<std::size_t>(kUvNodes * kUvNodes - 1);
  for (std::size_t layer = 0; layer < 3; ++layer) {
    EXPECT_EQ(result->uv_exposure[layer], 0.0);
    EXPECT_EQ(result->uv_exposure[last * 3 + layer], 0.0);
  }
}

TEST_F(FilmPrintTablesTest, PrintRgbaHasRedVaryingFastestAndKeepsTheValues) {
  FilmPrintTables tables(FilmStockBundle::Open(Resources()));
  const auto      result = tables.Resolve("neg_a", "paper_a", "reference");
  ASSERT_EQ(result->edge, kFilmPrintTableEdge);
  constexpr auto kEdge = static_cast<std::size_t>(kFilmPrintTableEdge);
  ASSERT_EQ(result->print_rgba.size(), kEdge * kEdge * kEdge * 4 * sizeof(float));
  const auto* rgba = reinterpret_cast<const float*>(result->print_rgba.data());
  for (const auto& [r, g, b] :
       {std::array<std::size_t, 3>{0, 0, 0}, std::array<std::size_t, 3>{7, 0, 0},
        std::array<std::size_t, 3>{0, 11, 0}, std::array<std::size_t, 3>{0, 0, 13},
        std::array<std::size_t, 3>{64, 32, 5}}) {
    const auto* voxel  = rgba + ((b * kEdge + g) * kEdge + r) * 4;
    const auto  source = ((r * kEdge + g) * kEdge + b) * 3;
    EXPECT_EQ(voxel[0], static_cast<float>(result->print_cube_root_xyz[source + 0]));
    EXPECT_EQ(voxel[1], static_cast<float>(result->print_cube_root_xyz[source + 1]));
    EXPECT_EQ(voxel[2], static_cast<float>(result->print_cube_root_xyz[source + 2]));
    EXPECT_EQ(voxel[3], 1.0f);
  }
}

TEST_F(FilmPrintTablesTest, DarkSurroundMovesLuminanceAndKeepsChromaticity) {
  constexpr double kExponent = 0.5;
  WriteSyntheticResources(root_ / "dark_surround", kExponent);
  FilmPrintTables plain_tables(FilmStockBundle::Open(Resources()));
  FilmPrintTables dark_tables(FilmStockBundle::Open(root_ / "dark_surround"));
  const auto      plain = plain_tables.Resolve("neg_a", "paper_a", "reference");
  const auto      dark  = dark_tables.Resolve("neg_a", "paper_a", "reference");
  ASSERT_EQ(plain->print_cube_root_xyz.size(), dark->print_cube_root_xyz.size());
  for (const std::size_t node : {std::size_t{0}, std::size_t{4000}, std::size_t{100000}}) {
    SCOPED_TRACE(node);
    const auto* before = plain->print_cube_root_xyz.data() + node * 3;
    const auto* after  = dark->print_cube_root_xyz.data() + node * 3;
    const auto  cube   = [](double value) { return value * value * value; };
    // The synthetic clear print has Y = 100.
    EXPECT_NEAR(cube(after[1]) / 100.0, std::pow(cube(before[1]) / 100.0, kExponent), 1e-9);
    EXPECT_NEAR(after[0] / after[1], before[0] / before[1], 1e-12);
    EXPECT_NEAR(after[2] / after[1], before[2] / before[1], 1e-12);
  }
}

TEST_F(FilmPrintTablesTest, KernelValuesFollowTheLayoutOfTheNegative) {
  const auto      bundle = FilmStockBundle::Open(Resources());
  FilmPrintTables tables(bundle);
  const auto      with_inhibition = tables.Resolve("neg_a", "paper_a", "reference");
  const auto      layout = film_print_kernel_tables::LayoutOf(*bundle, "neg_a");
  EXPECT_EQ(layout.uv_values, static_cast<std::size_t>(kUvNodes * kUvNodes * 3));
  EXPECT_EQ(layout.curve_values, 12u);
  EXPECT_EQ(layout.inhibition_values, 9u);
  EXPECT_EQ(layout.facets, 2u);
  ASSERT_EQ(with_inhibition->kernel_values.size(), layout.ValueCount());
  const auto& values = with_inhibition->kernel_values;
  for (std::size_t index = 0; index < layout.uv_values; ++index) {
    EXPECT_EQ(values[index], static_cast<float>(with_inhibition->uv_exposure[index]));
  }
  EXPECT_EQ(values[layout.CurvesOffset() + 1], 0.4f);
  EXPECT_EQ(values[layout.InhibitionOffset() + 8], 0.8f);
  EXPECT_EQ(values[layout.NormalsOffset() + 3], 1.0f);
  EXPECT_EQ(values[layout.RoomOffset() + 1], 5.0f);

  // neg_b has no inhibition.
  const auto no_inhibition = film_print_kernel_tables::LayoutOf(*bundle, "neg_b");
  EXPECT_EQ(no_inhibition.inhibition_values, 0u);
  EXPECT_FALSE(bundle->Negative("neg_b").inhibition.Present());
  EXPECT_EQ(tables.Resolve("neg_b", "paper_a", "reference")->kernel_values.size(),
            no_inhibition.ValueCount());

  // The key follows the negative and the light, and not the print material.
  const auto other_paper = tables.Resolve("neg_a", "paper_b", "reference");
  const auto other_light = tables.Resolve("neg_a", "paper_a", "daylight");
  EXPECT_EQ(with_inhibition->kernel_key, other_paper->kernel_key);
  EXPECT_NE(with_inhibition->kernel_key, other_light->kernel_key);
}

TEST_F(FilmPrintTablesTest, KernelParamsSplitTheInhibitionIntoAnImagePartAndAPointPart) {
  const auto       bundle = FilmStockBundle::Open(Resources());
  FilmKernelParams params{};
  film_print_kernel_tables::FillParams(*bundle, "neg_a", "paper_a", "reference", 3, params);
  EXPECT_EQ(params.inhibition_k[0], 0.1f);
  EXPECT_EQ(params.inhibition_image_k[0], 0.05f);
  EXPECT_FLOAT_EQ(params.inhibition_point_k[0], 0.05f);
  EXPECT_EQ(params.inhibition_image_k[4], 0.0f);
  EXPECT_FLOAT_EQ(params.inhibition_point_k[4], 0.2f);
  EXPECT_EQ(params.inhibition_point_k[8], 0.0f);
  EXPECT_EQ(params.inhibition_has_image, 1U);
  EXPECT_EQ(params.inhibition_has_point, 1U);

  // neg_b has no inhibition.
  FilmKernelParams none{};
  film_print_kernel_tables::FillParams(*bundle, "neg_b", "paper_a", "reference", 3, none);
  EXPECT_EQ(none.inhibition_has_image, 0U);
  EXPECT_EQ(none.inhibition_has_point, 0U);
}

TEST_F(FilmPrintTablesTest, ReversalSelectionReadsTheValuesOfItsFilmAndLight) {
  const auto bundle = FilmStockBundle::Open(Resources());
  ASSERT_EQ(bundle->Reversals().size(), 1u);
  EXPECT_EQ(bundle->Reversals()[0].display_name, "Reversal A");
  ASSERT_EQ(bundle->ReversalListOrder().size(), 1u);
  EXPECT_TRUE(bundle->IsReversal("rev_a"));
  EXPECT_FALSE(bundle->IsReversal("neg_a"));

  const auto daylight = bundle->ReversalSelection("rev_a", "daylight");
  EXPECT_EQ(daylight.layer_gain, (std::array<double, 3>{1.7, 1.8, 1.9}));
  EXPECT_EQ(daylight.xyz_to_display[1], -1.5);
  EXPECT_EQ(daylight.scene_spectrum[0], 1.0);                                         // 'flat'
  EXPECT_EQ(bundle->ReversalSelection("rev_a", "reference").scene_spectrum[0], 0.5);  // 'ramp'

  const auto film = bundle->Reversal("rev_a");
  EXPECT_EQ(film.dye_max[2], 3.2);
  EXPECT_EQ(film.clear_luminance, 85.0);
  EXPECT_FALSE(film.record.inhibition.Present());
  EXPECT_FALSE(film.record.negative_grade);
  // The steps FP1 to FP5 read the record of the reversal film in the form of a negative.
  EXPECT_EQ(bundle->Film("rev_a").curves.density.data(), film.record.curves.density.data());
  EXPECT_EQ(bundle->Film("neg_a").curves.density.data(),
            bundle->Negative("neg_a").curves.density.data());
  EXPECT_EQ(bundle->LightTable().xyz_scale, 12.5);
  EXPECT_EQ(bundle->LightTable().surround_exponent, 0.89);

  EXPECT_THROW((void)bundle->Reversal("neg_a"), std::runtime_error);
  EXPECT_THROW((void)bundle->ReversalSelection("rev_a", "no_light"), std::runtime_error);
  EXPECT_THROW((void)bundle->Film("no_film"), std::runtime_error);
}

TEST_F(FilmPrintTablesTest, LightTableIsTheTransmittanceOfTheRemovedDyeUnderTheTableLight) {
  const auto      bundle = FilmStockBundle::Open(Resources());
  FilmPrintTables tables(bundle);
  const auto      result = tables.Resolve("rev_a", "paper_a", "reference");
  ASSERT_TRUE(result->light_table);
  const auto     film  = bundle->Reversal("rev_a");
  const auto     view  = bundle->LightTable();
  constexpr auto kEdge = static_cast<std::size_t>(kFilmPrintTableEdge);
  ASSERT_EQ(result->print_cube_root_xyz.size(), kEdge * kEdge * kEdge * 3);
  for (const auto& [r, g, b] :
       {std::array<std::size_t, 3>{0, 0, 0}, std::array<std::size_t, 3>{64, 64, 64},
        std::array<std::size_t, 3>{7, 30, 63}, std::array<std::size_t, 3>{64, 0, 12}}) {
    SCOPED_TRACE(std::to_string(r) + ", " + std::to_string(g) + ", " + std::to_string(b));
    const std::array<std::size_t, 3> node{r, g, b};
    std::array<double, 3>            amounts{};
    for (std::size_t k = 0; k < 3; ++k) {
      const double record = film.record.density_grid_stop[k] * static_cast<double>(node[k]) /
                            static_cast<double>(kEdge - 1);
      amounts[k] = (std::max)(film.dye_max[k] - record, 0.0);
    }
    for (std::size_t k = 0; k < 3; ++k) {
      double sum = 0.0;
      for (std::int64_t w = 0; w < kWavelengths; ++w) {
        const auto   index   = static_cast<std::size_t>(w);
        const double density = film.record.base[index] + amounts[0] * film.record.dyes[index] +
                               amounts[1] * film.record.dyes[kWavelengths + index] +
                               amounts[2] * film.record.dyes[2 * kWavelengths + index];
        sum += std::pow(10.0, -density) * view.viewing_weights[k * kWavelengths + index];
      }
      const auto value = result->print_cube_root_xyz[((r * kEdge + g) * kEdge + b) * 3 + k];
      EXPECT_NEAR(value, std::cbrt(view.xyz_scale * sum), 1e-12);
    }
  }
}

TEST_F(FilmPrintTablesTest, ReversalReadsNoPrintMaterialAndSharesItsLightTableAcrossLights) {
  FilmPrintTables tables(FilmStockBundle::Open(Resources()));
  const auto      reference = tables.Resolve("rev_a", "paper_a", "reference");
  // The print material is not read: another one gives the same data with no build.
  EXPECT_EQ(tables.Resolve("rev_a", "paper_b", "reference").get(), reference.get());
  EXPECT_EQ(tables.Resolve("rev_a", "no_paper", "reference").get(), reference.get());
  const auto daylight = tables.Resolve("rev_a", "paper_a", "daylight");
  EXPECT_NE(daylight.get(), reference.get());
  EXPECT_NE(daylight->uv_key, reference->uv_key);
  EXPECT_NE(daylight->kernel_key, reference->kernel_key);
  // The light table depends on the film only.
  EXPECT_EQ(daylight->print_key, reference->print_key);
  EXPECT_EQ(daylight->print_cube_root_xyz, reference->print_cube_root_xyz);
  const auto stats = tables.Stats();
  EXPECT_EQ(stats.light_table_builds, 1);
  EXPECT_EQ(stats.uv_builds, 2);
  EXPECT_EQ(stats.negative_stage_builds, 0);
  EXPECT_EQ(stats.paper_stage_builds, 0);
  EXPECT_EQ(stats.compose_builds, 0);
  // A negative has another print key.
  EXPECT_NE(tables.Resolve("neg_a", "paper_a", "reference")->print_key, reference->print_key);
}

TEST_F(FilmPrintTablesTest, ReversalKernelParamsHaveTheLightTableForm) {
  const auto       bundle = FilmStockBundle::Open(Resources());
  FilmKernelParams params{};
  film_print_kernel_tables::FillParams(*bundle, "rev_a", "no_paper", "daylight", 65, params);
  EXPECT_EQ(params.light_table, 1U);
  EXPECT_EQ(params.cinema_look, 0U);
  EXPECT_EQ(params.negative_grade, 0U);
  EXPECT_EQ(params.inhibition_stored, 0U);
  EXPECT_FLOAT_EQ(params.layer_gain[2], 1.9f);
  EXPECT_FLOAT_EQ(params.xyz_to_display[0], 3.2f);
  EXPECT_FLOAT_EQ(params.display_surround_power, 0.89f - 1.0f);
  EXPECT_FLOAT_EQ(params.display_luminance[1], 0.7152f);
  EXPECT_FLOAT_EQ(params.print_start[0], 0.0f);
  EXPECT_FLOAT_EQ(params.print_step[2], 3.2f / 64.0f);

  // A print has the surround in its print table, so its params have none.
  FilmKernelParams print{};
  film_print_kernel_tables::FillParams(*bundle, "neg_a", "paper_a", "daylight", 65, print);
  EXPECT_EQ(print.light_table, 0U);
  EXPECT_EQ(print.display_surround_power, 0.0f);
}

TEST_F(FilmPrintTablesTest, NegativeScanSelectionReadsTheValuesOfItsNegativeAndLight) {
  const auto bundle = FilmStockBundle::Open(Resources());
  EXPECT_EQ(bundle->NegativeScan().id, "negative_scan");
  EXPECT_EQ(bundle->NegativeScan().display_name, "Negative Scan");
  EXPECT_TRUE(bundle->IsNegativeScan("negative_scan"));
  EXPECT_FALSE(bundle->IsNegativeScan("paper_a"));
  // Negative Scan is not a print material of the bundle.
  EXPECT_THROW((void)bundle->PrintMaterial("negative_scan"), std::runtime_error);

  // neg_b is the second negative and daylight the second light.
  const auto scan = bundle->NegativeScanSelection("neg_b", "daylight");
  EXPECT_EQ(scan.medium, "photo");
  EXPECT_EQ(scan.balance_light, "flat");
  EXPECT_EQ(scan.base_density, (std::array<double, 3>{0.2 * 1.2, 0.3 * 1.2, 0.4 * 1.2}));
  EXPECT_EQ(scan.aim_density, (std::array<double, 3>{0.82, 0.87, 0.92}));
  EXPECT_EQ(scan.light_axis, (std::array<double, 3>{0.13, 0.03, -0.23}));
  EXPECT_EQ(scan.gamma, (std::array<double, 3>{0.56, 0.57, 0.62}));
  // The layer gain and the scene spectrum are those of a print of the same negative and light.
  const auto print = bundle->Selection("neg_b", "paper_a", "daylight");
  EXPECT_EQ(scan.layer_gain, print.layer_gain);
  EXPECT_EQ(scan.scene_spectrum, print.scene_spectrum);
  EXPECT_EQ(bundle->LayerGain("neg_b", "daylight"), print.layer_gain);
  EXPECT_EQ(bundle->NegativeScanSelection("neg_a", "daylight").gamma,
            (std::array<double, 3>{0.48, 0.55, 0.53}));
  EXPECT_EQ(bundle->ScanMedium("neg_a").id, "cine");
  EXPECT_EQ(bundle->ScanBalanceLight("neg_a"), "ramp");
  EXPECT_EQ(bundle->LightSpectrum("ramp")[1], 0.75);
  EXPECT_THROW((void)bundle->LightSpectrum("D65"), std::runtime_error);

  const auto& constants = bundle->NegativeScanConstants();
  EXPECT_EQ(constants.centres_nm, (std::array<double, 3>{650.0, 545.0, 450.0}));
  EXPECT_EQ(constants.width_nm, 40.0);
  EXPECT_EQ(constants.band_cut, 1e-4);
  EXPECT_EQ(constants.lamp_kelvin, 3200.0);
  EXPECT_EQ(constants.planck_c2_nm_k, 1.4388e7);
  EXPECT_EQ(constants.default_light, "halogen");
  EXPECT_EQ(constants.axis_lights[0], "ramp");
  EXPECT_EQ(constants.axis_lights[1], "flat");
  ASSERT_EQ(constants.media.size(), 2U);
  EXPECT_EQ(constants.media[1].id, "photo");
  EXPECT_EQ(constants.media[1].pool, (std::vector<std::string>{"neg_a", "neg_b"}));
  EXPECT_EQ(constants.media[1].pool_scene_lights,
            (std::vector<std::string>{"reference", "daylight"}));
  EXPECT_EQ(constants.media[1].gamma[2], 0.62);

  EXPECT_THROW((void)bundle->NegativeScanSelection("rev_a", "daylight"), std::runtime_error);
  EXPECT_THROW((void)bundle->NegativeScanSelection("neg_a", "no_light"), std::runtime_error);
}

TEST_F(FilmPrintTablesTest, AsShotBlendsTheTwoGridLightsLinearInMired) {
  const auto bundle = FilmStockBundle::Open(Resources());
  ASSERT_EQ(bundle->CctGrid().kelvin, (std::vector<double>{3000.0, 6000.0}));
  // A named light is that light.
  const auto named = bundle->SceneLightMix("daylight", 4000.0);
  EXPECT_EQ(named.first, "daylight");
  EXPECT_EQ(named.second, "daylight");
  EXPECT_EQ(named.share, 0.0);
  // As Shot with no RAW white balance is the reference.
  const auto raster = bundle->SceneLightMix("as_shot", std::nullopt);
  EXPECT_EQ(raster.first, "reference");
  EXPECT_EQ(raster.share, 0.0);
  // 4000 K is 250 mired, the middle of 333.3 and 166.7 mired.
  const auto middle = bundle->SceneLightMix("as_shot", 4000.0);
  EXPECT_EQ(middle.first, "reference");
  EXPECT_EQ(middle.second, "daylight");
  EXPECT_NEAR(middle.share, 0.5, 1e-12);
  EXPECT_NEAR(bundle->SceneLightMix("as_shot", 1e6 / 300.0).share,
              (1e6 / 3000.0 - 300.0) / (1e6 / 3000.0 - 1e6 / 6000.0), 1e-12);
  // Outside the grid: the nearest end.
  EXPECT_EQ(bundle->SceneLightMix("as_shot", 2000.0).first, "reference");
  EXPECT_EQ(bundle->SceneLightMix("as_shot", 2000.0).share, 0.0);
  EXPECT_EQ(bundle->SceneLightMix("as_shot", 15000.0).first, "daylight");
  EXPECT_THROW((void)bundle->SceneLightMix("no_light", 4000.0), std::runtime_error);
  EXPECT_THROW((void)bundle->SceneLightMix("as_shot", std::nan("")), std::runtime_error);
  EXPECT_THROW((void)bundle->SceneLightMix("as_shot", 0.0), std::runtime_error);

  // Each value that depends on the light is the blend of its values under the two lights.
  const auto mix = FilmSceneLightMix{"reference", "daylight", 0.25};
  const auto a   = bundle->Selection("neg_b", "paper_b", "reference");
  const auto b   = bundle->Selection("neg_b", "paper_b", "daylight");
  const auto ab  = bundle->Selection("neg_b", "paper_b", mix);
  for (std::size_t k = 0; k < 3; ++k) {
    EXPECT_NEAR(ab.layer_gain[k], 0.75 * a.layer_gain[k] + 0.25 * b.layer_gain[k], 1e-15);
    EXPECT_NEAR(ab.printer_balance_log10[k],
                0.75 * a.printer_balance_log10[k] + 0.25 * b.printer_balance_log10[k], 1e-15);
    EXPECT_EQ(ab.printing_gain[k], a.printing_gain[k]);
  }
  ASSERT_EQ(ab.scene_spectrum.size(), a.scene_spectrum.size());
  for (std::size_t w = 0; w < ab.scene_spectrum.size(); ++w) {
    EXPECT_NEAR(ab.scene_spectrum[w], 0.75 * a.scene_spectrum[w] + 0.25 * b.scene_spectrum[w],
                1e-15);
  }
  // Negative Scan: D_aim, the light axis, and the layer gain are blends; D_base and gamma do not
  // depend on the light.
  const auto sa  = bundle->NegativeScanSelection("neg_a", "reference");
  const auto sb  = bundle->NegativeScanSelection("neg_a", "daylight");
  const auto sab = bundle->NegativeScanSelection("neg_a", mix);
  for (std::size_t k = 0; k < 3; ++k) {
    EXPECT_NEAR(sab.aim_density[k], 0.75 * sa.aim_density[k] + 0.25 * sb.aim_density[k], 1e-15);
    EXPECT_NEAR(sab.light_axis[k], 0.75 * sa.light_axis[k] + 0.25 * sb.light_axis[k], 1e-15);
    EXPECT_NEAR(sab.layer_gain[k], 0.75 * sa.layer_gain[k] + 0.25 * sb.layer_gain[k], 1e-15);
    EXPECT_EQ(sab.base_density[k], sa.base_density[k]);
    EXPECT_EQ(sab.gamma[k], sa.gamma[k]);
  }
  EXPECT_NEAR(sab.scene_spectrum[2], 0.75 * sa.scene_spectrum[2] + 0.25 * sb.scene_spectrum[2],
              1e-15);
  const auto ra  = bundle->ReversalSelection("rev_a", "reference");
  const auto rb  = bundle->ReversalSelection("rev_a", "daylight");
  const auto rab = bundle->ReversalSelection("rev_a", mix);
  EXPECT_NEAR(rab.layer_gain[1], 0.75 * ra.layer_gain[1] + 0.25 * rb.layer_gain[1], 1e-15);
  EXPECT_NEAR(rab.scene_spectrum[0], 0.75 * ra.scene_spectrum[0] + 0.25 * rb.scene_spectrum[0],
              1e-15);
}

TEST_F(FilmPrintTablesTest, UvTableOfABlendIsTheBlendOfTheTables) {
  // The layer exposure is linear in the light, so the table of the blended spectrum is the blend.
  FilmPrintTables tables(FilmStockBundle::Open(Resources()));
  const auto      a     = tables.Resolve("neg_a", "paper_a", "reference");
  const auto      b     = tables.Resolve("neg_a", "paper_a", "daylight");
  const auto      mix   = FilmSceneLightMix{"reference", "daylight", 0.4};
  const auto      blend = tables.Resolve("neg_a", "paper_a", mix);
  ASSERT_EQ(blend->uv_exposure.size(), a->uv_exposure.size());
  for (std::size_t index = 0; index < blend->uv_exposure.size(); ++index) {
    const auto expected = 0.6 * a->uv_exposure[index] + 0.4 * b->uv_exposure[index];
    EXPECT_NEAR(blend->uv_exposure[index], expected, 1e-12 * (std::max)(1.0, expected));
  }
  EXPECT_NE(blend->uv_key, a->uv_key);
  EXPECT_NE(blend->uv_key, b->uv_key);
  EXPECT_NE(blend->print_key, a->print_key);
  EXPECT_EQ(tables.Resolve("neg_a", "paper_a", mix).get(), blend.get());
  // A share of 0 is the first light, with its own key, and the same values.
  const auto zero =
      tables.Resolve("neg_a", "paper_a", FilmSceneLightMix{"reference", "daylight", 0.0});
  EXPECT_EQ(zero->uv_exposure, a->uv_exposure);
  // The Negative Scan of the blend shares the u'v' table of the print of the blend, and its scanner
  // table does not depend on the light.
  const auto scan = tables.Resolve("neg_a", "negative_scan", mix);
  EXPECT_EQ(scan->uv_key, blend->uv_key);
  EXPECT_EQ(scan->print_key, tables.Resolve("neg_a", "negative_scan", "reference")->print_key);
}

/// The weights of the lab rule 'ideal_scanner_bands' at @p nm, before the cut.
auto BandOf(double nm, double centre, double width) -> double {
  const double sigma = width / (2.0 * std::sqrt(2.0 * std::log(2.0)));
  const double z     = (nm - centre) / sigma;
  return std::exp(-0.5 * z * z);
}

TEST_F(FilmPrintTablesTest, ScannerWeightsAreGaussianBandsTimesTheLamp) {
  const auto  bundle    = FilmStockBundle::Open(Resources());
  const auto& constants = bundle->NegativeScanConstants();
  const auto  scanner   = film_negative_scan::DefaultScanner(constants);
  EXPECT_EQ(scanner.centres_nm, constants.centres_nm);
  EXPECT_EQ(scanner.width_nm, 40.0);
  EXPECT_FALSE(scanner.equal_energy);
  const auto weights =
      film_negative_scan::MakeScannerWeights(scanner, constants, bundle->WavelengthsNm());
  // Each wavelength of the bundle is in a band.
  EXPECT_EQ(weights.first_node, 0);
  EXPECT_EQ(weights.nodes, kWavelengths);
  ASSERT_EQ(weights.weights.size(), static_cast<std::size_t>(3 * kWavelengths));
  for (std::size_t k = 0; k < 3; ++k) {
    for (std::size_t w = 0; w < kWavelengthsNm.size(); ++w) {
      const double nm   = kWavelengthsNm[w];
      const double band = BandOf(nm, constants.centres_nm[k], 40.0);
      const double lamp = std::pow(nm, -5.0) / std::expm1(1.4388e7 / (nm * 3200.0));
      const double expected = band >= 1e-4 ? band * lamp : 0.0;
      EXPECT_NEAR(weights.weights[k * kWavelengths + w], expected, 1e-14 * expected)
          << k << ", " << nm;
    }
  }
  // The green band is below the cut at 400 nm, so its weight there is 0.
  EXPECT_EQ(weights.weights[kWavelengths], 0.0);
  EXPECT_GT(weights.weights[2 * kWavelengths], 0.0);

  // An equal-energy light has a lamp of 1.
  auto flat = scanner;
  flat.equal_energy = true;
  const auto flat_weights =
      film_negative_scan::MakeScannerWeights(flat, constants, bundle->WavelengthsNm());
  EXPECT_NEAR(flat_weights.weights[5], BandOf(700.0, 650.0, 40.0), 1e-15);
  // A band of 10 nm keeps the nodes from the first to the last wavelength in a band: 460 to 640 nm.
  // The green band has no node, so the scanner table of this scanner stops.
  auto narrow     = scanner;
  narrow.width_nm = 10.0;
  const auto narrow_weights =
      film_negative_scan::MakeScannerWeights(narrow, constants, bundle->WavelengthsNm());
  EXPECT_EQ(narrow_weights.first_node, 1);
  EXPECT_EQ(narrow_weights.nodes, 4);
  EXPECT_EQ(narrow_weights.weights[0], 0.0);
  FilmPrintTables tables(bundle);
  try {
    (void)tables.ResolveNegativeScan("neg_a", FilmSceneLightMix{"reference", "reference", 0.0},
                                     narrow);
    ADD_FAILURE() << "a band with no weight did not stop";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("no weight"), std::string::npos) << error.what();
  }

  // The lab limits of the scanner.
  for (const double width : {4.9, 150.1, std::nan("")}) {
    auto wrong     = scanner;
    wrong.width_nm = width;
    EXPECT_THROW((void)film_negative_scan::MakeScannerWeights(wrong, constants,
                                                              bundle->WavelengthsNm()),
                 std::runtime_error)
        << width;
  }
  for (const auto& centres :
       {std::array<double, 3>{545.0, 650.0, 450.0}, std::array<double, 3>{650.0, 545.0, 379.0},
        std::array<double, 3>{761.0, 545.0, 450.0}, std::array<double, 3>{650.0, 545.0, 545.0}}) {
    auto wrong       = scanner;
    wrong.centres_nm = centres;
    EXPECT_THROW((void)film_negative_scan::MakeScannerWeights(wrong, constants,
                                                              bundle->WavelengthsNm()),
                 std::runtime_error);
  }
  // The scanner of the document settings.
  FilmNegativeScanParams params;
  params.scanner_red_nm   = 630;
  params.scanner_green_nm = 530;
  params.scanner_blue_nm  = 460;
  params.scanner_width_nm = 60;
  params.scanner_light    = "equal_energy";
  const auto of           = film_negative_scan::ScannerOf(params);
  EXPECT_EQ(of.centres_nm, (std::array<double, 3>{630.0, 530.0, 460.0}));
  EXPECT_EQ(of.width_nm, 60.0);
  EXPECT_TRUE(of.equal_energy);
  EXPECT_EQ(film_negative_scan::ScannerOf(FilmNegativeScanParams{}), scanner);
  params.scanner_light = "led";
  EXPECT_THROW((void)film_negative_scan::ScannerOf(params), std::runtime_error);
}

TEST_F(FilmPrintTablesTest, ScannerTableIsTheScanDensityOfTheNegative) {
  const auto      bundle = FilmStockBundle::Open(Resources());
  FilmPrintTables tables(bundle);
  const auto      result = tables.Resolve("neg_a", "negative_scan", "reference");
  ASSERT_TRUE(result->scan);
  EXPECT_FALSE(result->light_table);
  const auto     negative = bundle->Negative("neg_a");
  const auto     weights  = film_negative_scan::MakeScannerWeights(
      film_negative_scan::DefaultScanner(bundle->NegativeScanConstants()), bundle->NegativeScanConstants(),
      bundle->WavelengthsNm());
  const auto&    w_values = weights.weights;
  constexpr auto kEdge    = static_cast<std::size_t>(kFilmPrintTableEdge);
  ASSERT_EQ(result->print_cube_root_xyz.size(), kEdge * kEdge * kEdge * 3);
  // T = log10(sum w) - log10(sum w 10^-(base + dyes a)), on the dye amounts of the nodes.
  const auto scan_density = [&](const std::array<double, 3>& amounts, std::size_t k) {
    double sum         = 0.0;
    double transmitted = 0.0;
    for (std::int64_t w = 0; w < kWavelengths; ++w) {
      const auto   index   = static_cast<std::size_t>(w);
      const double density = negative.base[index] + amounts[0] * negative.dyes[index] +
                             amounts[1] * negative.dyes[kWavelengths + index] +
                             amounts[2] * negative.dyes[2 * kWavelengths + index];
      transmitted += std::pow(10.0, -density) * w_values[k * kWavelengths + index];
      sum += w_values[k * kWavelengths + index];
    }
    return std::log10(sum) - std::log10(transmitted);
  };
  for (const auto& [r, g, b] :
       {std::array<std::size_t, 3>{0, 0, 0}, std::array<std::size_t, 3>{64, 64, 64},
        std::array<std::size_t, 3>{7, 30, 63}, std::array<std::size_t, 3>{64, 0, 12}}) {
    SCOPED_TRACE(std::to_string(r) + ", " + std::to_string(g) + ", " + std::to_string(b));
    const std::array<std::size_t, 3> node{r, g, b};
    std::array<double, 3>            amounts{};
    for (std::size_t k = 0; k < 3; ++k) {
      amounts[k] = negative.density_grid_start[k] +
                   (negative.density_grid_stop[k] - negative.density_grid_start[k]) *
                       static_cast<double>(node[k]) / static_cast<double>(kEdge - 1);
    }
    for (std::size_t k = 0; k < 3; ++k) {
      const auto value = result->print_cube_root_xyz[((r * kEdge + g) * kEdge + b) * 3 + k];
      EXPECT_NEAR(value, scan_density(amounts, k), 1e-12);
    }
  }
  // D_base is T of no dye.
  const auto base = film_negative_scan::BaseDensity(weights, negative.base);
  for (std::size_t k = 0; k < 3; ++k) {
    EXPECT_NEAR(base[k], scan_density({0.0, 0.0, 0.0}, k), 1e-14);
  }
  // The GPU form has the same values, with red varying fastest.
  const auto* rgba = reinterpret_cast<const float*>(result->print_rgba.data());
  EXPECT_FLOAT_EQ(rgba[((12 * kEdge + 0) * kEdge + 64) * 4 + 1],
                  static_cast<float>(result->print_cube_root_xyz[((64 * kEdge + 0) * kEdge + 12) * 3 + 1]));
}

TEST_F(FilmPrintTablesTest, ScannerTableReadIsTrilinearWithTheNearestEdge) {
  const auto      bundle = FilmStockBundle::Open(Resources());
  FilmPrintTables tables(bundle);
  const auto      result   = tables.Resolve("neg_b", "negative_scan", "reference");
  const auto      negative = bundle->Negative("neg_b");
  const film_negative_scan::ScannerTableView table{result->print_cube_root_xyz, kFilmPrintTableEdge};
  const auto node_value = [&](std::size_t r, std::size_t g, std::size_t b, std::size_t k) {
    return result->print_cube_root_xyz[((r * 65 + g) * 65 + b) * 3 + k];
  };
  const auto record_of = [&](double r, double g, double b) {
    std::array<double, 3> record{};
    const std::array<double, 3> position{r, g, b};
    for (std::size_t k = 0; k < 3; ++k) {
      record[k] = negative.density_grid_start[k] +
                  (negative.density_grid_stop[k] - negative.density_grid_start[k]) * position[k] / 64.0;
    }
    return record;
  };
  const auto on_node = film_negative_scan::ReadScannerTable(table, negative, record_of(10, 20, 30));
  const auto mid     = film_negative_scan::ReadScannerTable(table, negative, record_of(10, 20, 30.5));
  const auto low     = film_negative_scan::ReadScannerTable(table, negative, record_of(-3, -1, -2));
  const auto high    = film_negative_scan::ReadScannerTable(table, negative, record_of(70, 64, 99));
  for (std::size_t k = 0; k < 3; ++k) {
    EXPECT_NEAR(on_node[k], node_value(10, 20, 30, k), 1e-12);
    EXPECT_NEAR(mid[k], 0.5 * (node_value(10, 20, 30, k) + node_value(10, 20, 31, k)), 1e-12);
    EXPECT_NEAR(low[k], node_value(0, 0, 0, k), 1e-12);
    EXPECT_NEAR(high[k], node_value(64, 64, 64, k), 1e-12);
  }
}

TEST_F(FilmPrintTablesTest, PointRecordReadsTheCurvesAndTheInhibition) {
  const auto bundle = FilmStockBundle::Open(Resources());
  // neg_b has no inhibition: the record is the curve at criterion + log10 q. Each exposure is the
  // middle of two curve nodes: red -2.75, green -2.25, blue -1.75.
  const auto plain  = bundle->Negative("neg_b");
  const auto record = film_negative_scan::PointRecord(
      plain, {std::pow(10.0, -1.75), std::pow(10.0, -1.15), std::pow(10.0, -0.55)});
  EXPECT_NEAR(record[0], 0.5 * (0.1 + 0.4 * 1.2), 1e-12);
  EXPECT_NEAR(record[1], 0.5 * (0.1 + 0.5), 1e-12);
  EXPECT_NEAR(record[2], 0.5 * (0.1 + 0.3), 1e-12);
  // Outside a curve: its end values, as numpy.interp. The blue curve has 3 nodes.
  const auto ends = film_negative_scan::PointRecord(plain, {1e-9, 1e9, 1e9});
  EXPECT_NEAR(ends[0], 0.1, 1e-12);
  EXPECT_NEAR(ends[1], 1.6, 1e-12);
  EXPECT_NEAR(ends[2], 0.8, 1e-12);
  // neg_a has the inhibition: the record differs from its curve read.
  const auto inhibited = bundle->Negative("neg_a");
  const auto curve     = film_negative_scan::PointRecord(plain, {0.02, 0.03, 0.05});
  const auto with      = film_negative_scan::PointRecord(inhibited, {0.02, 0.03, 0.05});
  EXPECT_TRUE(std::ranges::all_of(with, [](double v) { return std::isfinite(v); }));
  EXPECT_NE(with, curve);
}

TEST_F(FilmPrintTablesTest, ScanReadsNoPaperAndSharesTheTablesOfTheNegativeWithAPrint) {
  const auto      bundle = FilmStockBundle::Open(Resources());
  FilmPrintTables tables(bundle);
  const auto      scan  = tables.Resolve("neg_a", "negative_scan", "reference");
  const auto      print = tables.Resolve("neg_a", "paper_a", "reference");
  // A change between Negative Scan and a print material keeps the GPU copies of the u'v' table and
  // of the kernel values, which have the same content.
  EXPECT_EQ(scan->uv_key, print->uv_key);
  EXPECT_EQ(scan->kernel_key, print->kernel_key);
  EXPECT_EQ(scan->uv_exposure, print->uv_exposure);
  EXPECT_EQ(scan->kernel_values, print->kernel_values);
  EXPECT_NE(scan->print_key, print->print_key);
  // The default scanner gives the stored values of the selection.
  EXPECT_EQ(scan->negative_scan, film_negative_scan::StoredSelectionValues(
                                     bundle->NegativeScanSelection("neg_a", "reference")));
  // The scanner table depends on the negative only.
  const auto daylight = tables.Resolve("neg_a", "negative_scan", "daylight");
  EXPECT_NE(daylight.get(), scan.get());
  EXPECT_EQ(daylight->print_key, scan->print_key);
  EXPECT_NE(daylight->negative_scan, scan->negative_scan);
  EXPECT_NE(tables.Resolve("neg_b", "negative_scan", "reference")->print_key, scan->print_key);
  EXPECT_EQ(tables.Resolve("neg_a", "negative_scan", "reference").get(), scan.get());
  const auto stats = tables.Stats();
  EXPECT_EQ(stats.scan_table_builds, 2);
  EXPECT_EQ(stats.scan_gamma_derivations, 0);
  EXPECT_EQ(stats.compose_builds, 1);
  EXPECT_EQ(stats.light_table_builds, 0);
  // A colour reversal film reads no print material, so a stored Negative Scan gives the light
  // table.
  const auto reversal = tables.Resolve("rev_a", "negative_scan", "reference");
  EXPECT_TRUE(reversal->light_table);
  EXPECT_FALSE(reversal->scan);
  EXPECT_EQ(reversal.get(), tables.Resolve("rev_a", "paper_a", "reference").get());
}

TEST_F(FilmPrintTablesTest, AnotherScannerDerivesTheValuesOfTheSelectionAgain) {
  const auto      bundle = FilmStockBundle::Open(Resources());
  FilmPrintTables tables(bundle);
  const film_negative_scan::Scanner other{{630.0, 530.0, 460.0}, 60.0, true};
  const auto      daylight = FilmSceneLightMix{"daylight", "daylight", 0.0};
  const auto      result   = tables.ResolveNegativeScan("neg_b", daylight, other);
  ASSERT_TRUE(result->scan);
  // The medium of neg_b is 'photo', whose pool has neg_a and neg_b: two scanner tables and one
  // derivation of gamma.
  auto stats = tables.Stats();
  EXPECT_EQ(stats.scan_table_builds, 2);
  EXPECT_EQ(stats.scan_gamma_derivations, 1);
  const auto weights = film_negative_scan::MakeScannerWeights(other, bundle->NegativeScanConstants(),
                                                              bundle->WavelengthsNm());
  // The values with the public steps: the pool stocks under their balance light with the layer
  // gain of their pool scene light, then D_aim of the balance light of neg_b and the light axis
  // from 'ramp' to 'flat' with the layer gain of the selection.
  const auto table_a =
      tables.ResolveNegativeScan("neg_a", FilmSceneLightMix{"reference", "reference", 0.0}, other);
  stats = tables.Stats();
  EXPECT_EQ(stats.scan_table_builds, 2);
  // The medium of neg_a is 'cine', so its gamma is derived for this scanner too.
  EXPECT_EQ(stats.scan_gamma_derivations, 2);
  const film_negative_scan::ScannerTableView view_a{table_a->print_cube_root_xyz, kFilmPrintTableEdge};
  const film_negative_scan::ScannerTableView view_b{result->print_cube_root_xyz, kFilmPrintTableEdge};
  const auto grey = [&](const std::string& negative, const film_negative_scan::ScannerTableView& view,
                        const std::array<double, 3>& gain, const std::string& light, double value) {
    return film_negative_scan::GreyDensity(
        *bundle, negative, view,
        film_negative_scan::BaseDensity(weights, bundle->Negative(negative).base), gain,
        bundle->LightSpectrum(light), value);
  };
  constexpr double kTwoStops = 2.0 * 0.30102999566398119521;
  const auto       gain_a    = bundle->LayerGain("neg_a", "reference");
  const auto       gain_b    = bundle->LayerGain("neg_b", "daylight");
  const auto       high_a    = grey("neg_a", view_a, gain_a, "ramp", 0.36);
  const auto       low_a     = grey("neg_a", view_a, gain_a, "ramp", 0.09);
  const auto       high_b    = grey("neg_b", view_b, gain_b, "flat", 0.36);
  const auto       low_b     = grey("neg_b", view_b, gain_b, "flat", 0.09);
  const auto&      values    = result->negative_scan;
  const auto       base_b    = film_negative_scan::BaseDensity(weights, bundle->Negative("neg_b").base);
  const auto       aim       = grey("neg_b", view_b, gain_b, "flat", 0.18);
  const auto       from      = grey("neg_b", view_b, gain_b, "ramp", 0.18);
  for (std::size_t k = 0; k < 3; ++k) {
    const double gamma =
        0.5 * ((high_a[k] - low_a[k]) / kTwoStops + (high_b[k] - low_b[k]) / kTwoStops);
    EXPECT_GT(gamma, 0.0);
    EXPECT_NEAR(values.gamma[k], gamma, 1e-12) << k;
    EXPECT_EQ(values.base_density[k], base_b[k]);
    EXPECT_NEAR(values.aim_density[k], aim[k], 1e-12) << k;
    EXPECT_NEAR(values.light_axis[k], (aim[k] - from[k]) / gamma, 1e-12) << k;
  }
  // Each value differs from the stored value of the default scanner.
  const auto stored = tables.Resolve("neg_b", "negative_scan", daylight);
  EXPECT_NE(stored->print_key, result->print_key);
  EXPECT_EQ(stored->uv_key, result->uv_key);
  EXPECT_NE(stored->negative_scan.gamma, values.gamma);
  // The same scanner again builds nothing.
  EXPECT_EQ(tables.ResolveNegativeScan("neg_b", daylight, other).get(), result.get());
  EXPECT_EQ(tables.Stats().scan_gamma_derivations, 2);
  // The default scanner through ResolveNegativeScan is the result of Resolve.
  EXPECT_EQ(tables
                .ResolveNegativeScan("neg_b", daylight,
                                     film_negative_scan::DefaultScanner(bundle->NegativeScanConstants()))
                .get(),
            stored.get());
}

TEST_F(FilmPrintTablesTest, ScanKernelParamsHaveTheScanForm) {
  const auto       bundle = FilmStockBundle::Open(Resources());
  const auto       light  = FilmSceneLightMix{"daylight", "daylight", 0.0};
  FilmKernelParams params{};
  film_print_kernel_tables::FillParams(*bundle, "neg_b", "negative_scan", light, 65, params);
  EXPECT_EQ(params.scan, 1U);
  EXPECT_EQ(params.light_table, 0U);
  EXPECT_EQ(params.cinema_look, 0U);
  EXPECT_EQ(params.negative_grade, 0U);
  EXPECT_EQ(params.display_surround_power, 0.0f);
  EXPECT_FLOAT_EQ(params.layer_gain[0], 1.9f);
  // The record grid of the scanner table is the grid of the print table.
  const auto negative = bundle->Negative("neg_b");
  EXPECT_FLOAT_EQ(params.print_step[1],
                  static_cast<float>((negative.density_grid_stop[1] - negative.density_grid_start[1]) / 64.0));
  // The other forms are not the scan form.
  FilmKernelParams print{};
  film_print_kernel_tables::FillParams(*bundle, "neg_b", "paper_a", light, 65, print);
  EXPECT_EQ(print.scan, 0U);
  FilmKernelParams reversal{};
  film_print_kernel_tables::FillParams(*bundle, "rev_a", "negative_scan", "daylight", 65, reversal);
  EXPECT_EQ(reversal.scan, 0U);
  EXPECT_EQ(reversal.light_table, 1U);
}

/// E' of the lab `NegativeScanPreview.positive` in double precision, written out.
auto LabPositive(const film_negative_scan::SelectionValues& values,
                 const FilmNegativeScanParams& params, const std::array<double, 3>& auto_shift,
                 const std::array<double, 3>& t) -> std::array<double, 3> {
  std::array<double, 3> out{};
  const std::array<double, 3> keys{params.cyan, params.magenta, params.yellow};
  for (std::size_t k = 0; k < 3; ++k) {
    const double shift = auto_shift[k] + params.exposure_stops * std::log10(2.0) - keys[k];
    const double x     = params.contrast * (t[k] - values.base_density[k] - values.aim_density[k]) /
                         values.gamma[k] + shift;
    const double x_base = params.contrast * (0.0 - values.aim_density[k]) / values.gamma[k] + shift;
    const double e      = 0.18 * std::pow(10.0, x);
    const double e_base = 0.18 * std::pow(10.0, x_base);
    out[k] = (std::max)(0.18 * (e - params.black * e_base) / (0.18 - params.black * e_base), 0.0);
  }
  return out;
}

TEST_F(FilmPrintTablesTest, RenderTermsTakeTheGreyToTheAimAndTheBaseToBlack) {
  const film_negative_scan::SelectionValues values{
      {0.2, 0.3, 0.4}, {0.9, 1.0, 1.1}, {0.1, 0.0, -0.2}, {0.5, 0.55, 0.6}};
  const auto run = [&values](const FilmNegativeScanParams& params,
                             const std::array<double, 3>& auto_shift,
                             const std::array<double, 3>& t) {
    const auto terms = film_negative_scan::MakeRenderTerms(values, params, auto_shift);
    std::array<double, 3> out{};
    for (std::size_t k = 0; k < 3; ++k) {
      out[k] = (std::max)(terms.scale[k] * (0.18 * std::pow(10.0, terms.slope[k] * t[k] +
                                                                      terms.offset[k]) -
                                            terms.floor[k]),
                          0.0);
    }
    return out;
  };
  const std::array<double, 3> grey{0.2 + 0.9, 0.3 + 1.0, 0.4 + 1.1};
  const std::array<double, 3> zero{};
  // The defaults: the grey of the balance light is 0.18, and the film base is 0 (black 1).
  const FilmNegativeScanParams defaults;
  for (std::size_t k = 0; k < 3; ++k) {
    EXPECT_NEAR(run(defaults, zero, grey)[k], 0.18, 1e-15) << k;
    EXPECT_NEAR(run(defaults, zero, values.base_density)[k], 0.0, 1e-15) << k;
  }
  // Each setting, against the formula of the lab.
  FilmNegativeScanParams graded;
  graded.contrast       = 1.2f;
  graded.exposure_stops = -0.75f;
  graded.cyan           = 0.05f;
  graded.magenta        = -0.03f;
  graded.yellow         = 0.11f;
  graded.black          = 0.6f;
  FilmNegativeScanParams no_black;
  no_black.contrast = 0.7f;
  no_black.black    = 0.0f;
  const std::array<double, 3> auto_shift{0.01, -0.02, 0.03};
  for (const auto& params : {defaults, graded, no_black}) {
    for (const auto& t : {grey, std::array<double, 3>{0.5, 2.0, 1.2}, std::array<double, 3>{0.25, 0.35, 0.45}}) {
      const auto got      = run(params, auto_shift, t);
      const auto expected = LabPositive(values, params, auto_shift, t);
      for (std::size_t k = 0; k < 3; ++k) {
        EXPECT_NEAR(got[k], expected[k], 1e-13 * (std::max)(1.0, expected[k])) << k;
      }
    }
  }
  // The float form of the kernels gives the same values to float precision.
  const auto terms = film_negative_scan::MakeRenderTerms(values, graded, auto_shift);
  float base[3]    = {0.2f, 0.3f, 0.4f};
  float aim[3]     = {0.9f, 1.0f, 1.1f};
  float gamma[3]   = {0.5f, 0.55f, 0.6f};
  const auto manual = film_negative_scan::ManualShift(graded);
  float shift[3];
  for (std::size_t k = 0; k < 3; ++k) shift[k] = static_cast<float>(manual[k] + auto_shift[k]);
  FilmKernelNegativeScan kernel{};
  ASSERT_EQ(FilmKernelNegativeScanTerms(base, aim, gamma, 1.2f, shift, 0.6f, &kernel), 1);
  for (std::size_t k = 0; k < 3; ++k) {
    EXPECT_NEAR(kernel.slope[k], terms.slope[k], 1e-5 * std::abs(terms.slope[k]));
    EXPECT_NEAR(kernel.floor[k], terms.floor[k], 1e-5 * std::abs(terms.floor[k]) + 1e-9);
  }
  const auto pixel   = FilmKernelNegativeScanPixel(FilmKernelMakeRgb(0.5f, 2.0f, 1.2f), &kernel);
  const auto lab     = LabPositive(values, graded, auto_shift, {0.5, 2.0, 1.2});
  EXPECT_NEAR(pixel.r, lab[0], 2e-5 * (std::max)(lab[0], 1e-3));
  EXPECT_NEAR(pixel.g, lab[1], 2e-5 * (std::max)(lab[1], 1e-3));
  EXPECT_NEAR(pixel.b, lab[2], 2e-5 * (std::max)(lab[2], 1e-3));
  // The prescan terms: x = contrast (D - D_aim) / gamma, so the grey is at 0.
  const auto prescan = film_negative_scan::MakePrescanTerms(values, graded);
  for (std::size_t k = 0; k < 3; ++k) {
    EXPECT_NEAR(prescan.slope[k] * grey[k] + prescan.offset[k], 0.0, 1e-15);
    EXPECT_NEAR(prescan.slope[k], 1.2 / values.gamma[k], 1e-6);
  }

  // A film base at or above the grey after the black stops with an error that names the black, as
  // the lab: contrast 0.5 and +3 stops put the film base above the grey.
  FilmNegativeScanParams bright;
  bright.contrast       = 0.5f;
  bright.exposure_stops = 3.0f;
  bright.black          = 1.5f;
  try {
    (void)film_negative_scan::MakeRenderTerms(values, bright, zero);
    ADD_FAILURE() << "a film base above the grey did not stop";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("Black"), std::string::npos) << error.what();
  }
  bright.black = 0.0f;
  EXPECT_NO_THROW((void)film_negative_scan::MakeRenderTerms(values, bright, zero));
}

TEST_F(FilmPrintTablesTest, PrescanIsTheBoxReductionOfPillow) {
  using film_negative_scan::PrescanShape;
  EXPECT_EQ(PrescanShape(257, 391), (std::array<std::int64_t, 2>{63, 96}));
  EXPECT_EQ(PrescanShape(100, 50), (std::array<std::int64_t, 2>{96, 48}));
  EXPECT_EQ(PrescanShape(10, 20), (std::array<std::int64_t, 2>{10, 20}));
  // Python round: 16.5 is 16, 17.5 is 18.
  EXPECT_EQ(PrescanShape(33, 192), (std::array<std::int64_t, 2>{16, 96}));
  EXPECT_EQ(PrescanShape(35, 192), (std::array<std::int64_t, 2>{18, 96}));
  EXPECT_EQ(PrescanShape(1, 4000), (std::array<std::int64_t, 2>{1, 96}));

  // A reduction by 2: each value is the mean of its 2 x 2 box, in float32.
  film_negative_scan::Frame frame;
  frame.rows    = 2;
  frame.columns = 192;
  for (std::int64_t y = 0; y < frame.rows; ++y) {
    for (std::int64_t x = 0; x < frame.columns; ++x) {
      frame.values.insert(frame.values.end(), {0.1 * static_cast<double>(x), static_cast<double>(y),
                                               -0.37});
    }
  }
  const auto reduced = film_negative_scan::ReduceFrame(frame);
  ASSERT_EQ(reduced.rows, 1);
  ASSERT_EQ(reduced.columns, 96);
  for (std::int64_t x = 0; x < 96; ++x) {
    const auto at = static_cast<std::size_t>(x * 3);
    EXPECT_NEAR(reduced.values[at], 0.1 * (2.0 * static_cast<double>(x) + 0.5), 1e-5);
    EXPECT_NEAR(reduced.values[at + 1], 0.5, 1e-7);
    EXPECT_EQ(reduced.values[at + 2], static_cast<double>(-0.37f));
  }
  // A frame no larger than the prescan is read as it is, in float64.
  film_negative_scan::Frame small{{0.1234567890123, 1.0, 2.0}, 1, 1};
  EXPECT_EQ(film_negative_scan::ReduceFrame(small).values, small.values);
}

TEST_F(FilmPrintTablesTest, AutoColourCorrectsTheCastAlongAndAcrossTheLightAxis) {
  // Each pixel has the cast (0.06, 0.0, -0.06) at level 0.1: the two estimates are that cast.
  film_negative_scan::Frame prescan;
  prescan.rows    = 4;
  prescan.columns = 8;
  for (int p = 0; p < 32; ++p) {
    prescan.values.insert(prescan.values.end(), {0.16, 0.1, 0.04});
  }
  const std::array<double, 3> axis{0.3, 0.05, -0.4};
  // The axis without its mean, of length 1.
  std::array<double, 3> unit = axis;
  const double          mean = (axis[0] + axis[1] + axis[2]) / 3.0;
  for (auto& v : unit) v -= mean;
  const double norm = std::sqrt(unit[0] * unit[0] + unit[1] * unit[1] + unit[2] * unit[2]);
  for (auto& v : unit) v /= norm;
  const std::array<double, 3> cast{0.06, 0.0, -0.06};
  const double along = cast[0] * unit[0] + cast[1] * unit[1] + cast[2] * unit[2];
  for (const auto& [mode, target] : {std::pair<std::string, double>{"neutral", 0.0},
                                     std::pair<std::string, double>{"warm", 0.04},
                                     std::pair<std::string, double>{"cool", -0.04}}) {
    SCOPED_TRACE(mode);
    const auto result = film_negative_scan::AutoColourShift(prescan, axis, mode, 0.8, 0.5);
    EXPECT_EQ(result.prescan_pixels, 32);
    for (std::size_t k = 0; k < 3; ++k) {
      EXPECT_NEAR(result.near_neutral_cast[k], cast[k], 1e-12);
      EXPECT_NEAR(result.white_patch_cast[k], cast[k], 1e-12);
      const double across = cast[k] - along * unit[k];
      const double shift  = -0.8 * (along - target) * unit[k] - 0.8 * 0.5 * across - 0.5 * 0.1;
      EXPECT_NEAR(result.shift[k], shift, 1e-12) << k;
    }
  }
  // Neutral with a strength of 1 and no density takes the cast away.
  const auto neutral = film_negative_scan::AutoColourShift(prescan, axis, "neutral", 1.0, 0.0);
  EXPECT_NEAR(neutral.shift[0] + cast[0] - (neutral.shift[1] + cast[1]), 0.0, 0.5 * 0.06 + 1e-12);
  // Off reads no pixel, so an empty prescan is valid.
  const auto off = film_negative_scan::AutoColourShift(film_negative_scan::Frame{}, axis, "off", 0.8, 0.0);
  EXPECT_EQ(off.shift, (std::array<double, 3>{}));
  EXPECT_EQ(off.prescan_pixels, 0);
  // Fewer than 16 pixels inside the valid range stop; the range is open.
  auto dark = prescan;
  for (std::size_t p = 0; p < 20; ++p) {
    dark.values[p * 3] = dark.values[p * 3 + 1] = dark.values[p * 3 + 2] = -1.6;
  }
  EXPECT_THROW((void)film_negative_scan::AutoColourShift(dark, axis, "neutral", 0.8, 0.0),
               std::runtime_error);
  EXPECT_THROW((void)film_negative_scan::AutoColourShift(prescan, axis, "vivid", 0.8, 0.0),
               std::runtime_error);
}

TEST_F(FilmPrintTablesTest, NegativeScanThatNamesAPrintMaterialThrows) {
  WriteSyntheticResources(root_ / "negative_scan_names_a_paper", 1.0, "paper_a");
  const auto error = OpenError(root_ / "negative_scan_names_a_paper");
  EXPECT_NE(error.find("Negative Scan"), std::string::npos) << error;
  EXPECT_NE(error.find("paper_a"), std::string::npos) << error;
}

TEST_F(FilmPrintTablesTest, ScanMediumGammaOfZeroThrows) {
  // x divides by gamma.
  WriteSyntheticResources(root_ / "scan_gamma_zero", 1.0, "negative_scan", 0.0);
  const auto error = OpenError(root_ / "scan_gamma_zero");
  EXPECT_NE(error.find("gamma"), std::string::npos) << error;
  EXPECT_NE(error.find("cine"), std::string::npos) << error;
}

TEST_F(FilmPrintTablesTest, NegativeOfAMediumWithNoScanValuesThrows) {
  WriteSyntheticResources(root_ / "scan_no_medium", 1.0, "negative_scan", 0.48, "slide");
  const auto error = OpenError(root_ / "scan_no_medium");
  EXPECT_NE(error.find("slide"), std::string::npos) << error;
  EXPECT_NE(error.find("neg_b"), std::string::npos) << error;
}

TEST_F(FilmPrintTablesTest, DisplaySurroundMovesLuminanceAndKeepsChromaticity) {
  FilmKernelParams params{};
  params.display_luminance[0]   = 0.2126f;
  params.display_luminance[1]   = 0.7152f;
  params.display_luminance[2]   = 0.0722f;
  params.display_surround_power = 0.89f - 1.0f;
  const auto  rgb               = FilmKernelMakeRgb(0.3f, 0.2f, 0.1f);
  const auto  out               = FilmKernelDisplaySurround(&params, rgb);
  const float y                 = 0.2126f * 0.3f + 0.7152f * 0.2f + 0.0722f * 0.1f;
  const float y2                = 0.2126f * out.r + 0.7152f * out.g + 0.0722f * out.b;
  EXPECT_NEAR(y2, std::pow(y, 0.89f), 1e-6f);
  EXPECT_NEAR(out.r / out.g, rgb.r / rgb.g, 1e-6f);
  EXPECT_NEAR(out.b / out.g, rgb.b / rgb.g, 1e-6f);
  // The clear film has a luminance of 1 and stays white.
  const auto white = FilmKernelDisplaySurround(&params, FilmKernelMakeRgb(1.0f, 1.0f, 1.0f));
  EXPECT_NEAR(white.g, 1.0f, 1e-6f);
  params.display_surround_power = 0.0f;
  EXPECT_EQ(FilmKernelDisplaySurround(&params, rgb).r, rgb.r);
}

TEST_F(FilmPrintTablesTest, StageTablesAreReused) {
  FilmPrintTables tables(FilmStockBundle::Open(Resources()));
  (void)tables.Resolve("neg_a", "paper_a", "reference");
  auto stats = tables.Stats();
  EXPECT_EQ(stats.negative_stage_builds, 1);
  EXPECT_EQ(stats.paper_stage_builds, 1);
  EXPECT_EQ(stats.compose_builds, 1);

  // A second selection with one paper builds no new stage B.
  (void)tables.Resolve("neg_b", "paper_a", "reference");
  stats = tables.Stats();
  EXPECT_EQ(stats.negative_stage_builds, 2);
  EXPECT_EQ(stats.paper_stage_builds, 1);
  EXPECT_EQ(stats.compose_builds, 2);

  // A new scene light repeats only the u'v' integral and the composition.
  (void)tables.Resolve("neg_b", "paper_a", "daylight");
  stats = tables.Stats();
  EXPECT_EQ(stats.negative_stage_builds, 2);
  EXPECT_EQ(stats.paper_stage_builds, 1);
  EXPECT_EQ(stats.uv_builds, 3);
  EXPECT_EQ(stats.compose_builds, 3);

  // A new paper builds stage A and stage B.
  (void)tables.Resolve("neg_b", "paper_b", "daylight");
  stats = tables.Stats();
  EXPECT_EQ(stats.negative_stage_builds, 3);
  EXPECT_EQ(stats.paper_stage_builds, 2);
}

TEST_F(FilmPrintTablesTest, SameSelectionReturnsTheSameDataWithNoBuild) {
  FilmPrintTables tables(FilmStockBundle::Open(Resources()));
  const auto      first  = tables.Resolve("neg_a", "paper_a", "reference");
  const auto      second = tables.Resolve("neg_a", "paper_a", "reference");
  EXPECT_EQ(first.get(), second.get());
  EXPECT_EQ(tables.Stats().uv_builds, 1);
  EXPECT_EQ(tables.Stats().compose_builds, 1);
}

TEST_F(FilmPrintTablesTest, KeysChangeWithTheSelectionAndStayStableAcrossOwners) {
  const auto      bundle = FilmStockBundle::Open(Resources());
  FilmPrintTables tables(bundle);
  const auto      base = tables.Resolve("neg_a", "paper_a", "reference");
  EXPECT_FALSE(base->print_key.Empty());
  EXPECT_FALSE(base->uv_key.Empty());
  const auto other_paper = tables.Resolve("neg_a", "paper_b", "reference");
  EXPECT_NE(base->print_key, other_paper->print_key);
  // The u'v' table does not depend on the paper.
  EXPECT_EQ(base->uv_key, other_paper->uv_key);
  EXPECT_EQ(base->uv_exposure, other_paper->uv_exposure);
  const auto other_light = tables.Resolve("neg_a", "paper_a", "daylight");
  EXPECT_NE(base->print_key, other_light->print_key);
  EXPECT_NE(base->uv_key, other_light->uv_key);

  FilmPrintTables second_owner(FilmStockBundle::Open(Resources()));
  const auto      again = second_owner.Resolve("neg_a", "paper_a", "reference");
  EXPECT_EQ(base->print_key, again->print_key);
  EXPECT_EQ(base->print_cube_root_xyz, again->print_cube_root_xyz);
}

TEST_F(FilmPrintTablesTest, ByteLimitRemovesTheOldestTablesAndKeepsReturnedDataValid) {
  // One result is about 12 MB and each stage about 7 MB, so this limit holds one stage only.
  constexpr std::size_t kLimit = 8ull << 20;
  FilmPrintTables       tables(FilmStockBundle::Open(Resources()), kLimit);
  const auto            first = tables.Resolve("neg_a", "paper_a", "reference");
  EXPECT_LE(tables.Stats().kept_bytes, kLimit);
  EXPECT_FALSE(first->print_rgba.empty());

  const auto expected = first->print_cube_root_xyz;
  const auto again    = tables.Resolve("neg_a", "paper_a", "reference");
  EXPECT_EQ(tables.Stats().compose_builds, 2);
  EXPECT_NE(first.get(), again.get());
  EXPECT_EQ(again->print_cube_root_xyz, expected);
  EXPECT_LE(tables.Stats().kept_bytes, kLimit);
}

TEST_F(FilmPrintTablesTest, ConcurrentCallersOfOneSelectionShareOneBuild) {
  FilmPrintTables tables(FilmStockBundle::Open(Resources()));
  std::array<std::shared_ptr<const FilmPrintTableData>, 4> results;
  std::vector<std::thread>                                 threads;
  for (auto& result : results) {
    threads.emplace_back(
        [&tables, &result] { result = tables.Resolve("neg_a", "paper_a", "reference"); });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(tables.Stats().compose_builds, 1);
  for (const auto& result : results) {
    EXPECT_EQ(result.get(), results[0].get());
  }
}

TEST_F(FilmPrintTablesTest, ResolveWritesNoFile) {
  const auto directory = CopyOfResources("no_new_file");
  const auto files     = [&directory] {
    std::vector<std::string> names;
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
      names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
  };
  const auto      before = files();
  FilmPrintTables tables(FilmStockBundle::Open(directory));
  (void)tables.Resolve("neg_b", "paper_b", "daylight");
  EXPECT_EQ(files(), before);
}

TEST_F(FilmPrintTablesTest, NullBundleIsRejected) {
  EXPECT_THROW(FilmPrintTables(nullptr), std::invalid_argument);
}

// The lab values come from build_product_resources.py of alcedo-film-lab. They are not in Git, so
// these tests need ALCEDO_FILM_PRINT_RESOURCE_DIR and ALCEDO_FILM_PRINT_EXPECTED_DIR at configure
// time. A skip here means that the comparison with the lab did not run.
#if defined(ALCEDO_FILM_PRINT_RESOURCE_DIR) && defined(ALCEDO_FILM_PRINT_EXPECTED_DIR)

constexpr double kLabRelativeLimit          = 1e-9;
// The largest relative rounding error of 12 mantissa bits is 1.22e-4.
constexpr double kCompactTableRelativeLimit = 2e-4;

/// Largest of |a - b| / max(|a|, |b|).
auto LargestRelativeDifference(std::span<const double> values, std::span<const double> expected)
    -> double {
  double largest = 0.0;
  for (std::size_t index = 0; index < values.size(); ++index) {
    const auto scale = (std::max)(std::fabs(values[index]), std::fabs(expected[index]));
    if (scale > 0.0) {
      largest = (std::max)(largest, std::fabs(values[index] - expected[index]) / scale);
    }
  }
  return largest;
}

/// One file of the lab values. A file of a colour reversal film names the film in `reversal`, has
/// no print material, and holds the light table in place of the print table.
struct LabSelection {
  fs::path    path;
  std::string negative;
  std::string print_material;
  std::string scene_light;
  bool        reversal = false;
  /// A file of print material Negative Scan holds the scanner table and the values of the
  /// selection, and no u'v' table.
  bool        scan     = false;
};

/// A file of the blend of two grid lights for scene light As Shot (`scan__<negative>__cct<K>.bin`).
struct LabBlend {
  fs::path    path;
  std::string negative;
  double      kelvin = 0.0;
  std::string node_a;
  std::string node_b;
};

auto LabBlends() -> std::vector<LabBlend> {
  std::vector<LabBlend> blends;
  for (const auto& entry : fs::directory_iterator(fs::path(ALCEDO_FILM_PRINT_EXPECTED_DIR))) {
    if (!entry.is_regular_file()) continue;
    const auto file     = FilmArrayFile::Parse(ReadBytes(entry.path()), entry.path());
    const auto metadata = file.Metadata();
    if (!metadata.contains("kelvin")) continue;
    blends.push_back({entry.path(), metadata.at("negative").get<std::string>(),
                      metadata.at("kelvin").get<double>(), metadata.at("node_a").get<std::string>(),
                      metadata.at("node_b").get<std::string>()});
  }
  return blends;
}

auto LabSelections() -> std::vector<LabSelection> {
  std::vector<LabSelection> selections;
  for (const auto& entry : fs::directory_iterator(fs::path(ALCEDO_FILM_PRINT_EXPECTED_DIR))) {
    // The subdirectory 'display' holds the values of the kernel tests.
    if (!entry.is_regular_file()) continue;
    const auto file     = FilmArrayFile::Parse(ReadBytes(entry.path()), entry.path());
    const auto metadata = file.Metadata();
    // A blend file has no table: see LabBlends. The files of a second scanner and of the auto
    // colour name no selection.
    if (metadata.contains("kelvin")) continue;
    if (!metadata.contains("reversal") && !metadata.contains("print_material")) continue;
    if (metadata.contains("reversal")) {
      selections.push_back({entry.path(), metadata.at("reversal").get<std::string>(), "",
                            metadata.at("scene_light").get<std::string>(), true});
      continue;
    }
    const auto material = metadata.at("print_material").get<std::string>();
    selections.push_back({entry.path(), metadata.at("negative").get<std::string>(), material,
                          metadata.at("scene_light").get<std::string>(), false,
                          material == kFilmNegativeScanId});
  }
  return selections;
}

/// The files with a u'v' table: the prints and the reversal films.
auto TableSelections() -> std::vector<LabSelection> {
  auto selections = LabSelections();
  std::erase_if(selections, [](const LabSelection& selection) { return selection.scan; });
  return selections;
}

auto PrintSelections() -> std::vector<LabSelection> {
  auto selections = TableSelections();
  std::erase_if(selections, [](const LabSelection& selection) { return selection.reversal; });
  return selections;
}

auto ScanSelections() -> std::vector<LabSelection> {
  auto selections = LabSelections();
  std::erase_if(selections, [](const LabSelection& selection) { return !selection.scan; });
  return selections;
}

auto ReversalSelections() -> std::vector<LabSelection> {
  auto selections = LabSelections();
  std::erase_if(selections, [](const LabSelection& selection) { return !selection.reversal; });
  return selections;
}

class FilmPrintTablesLabTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    bundle_ = FilmStockBundle::Open(fs::path(ALCEDO_FILM_PRINT_RESOURCE_DIR));
    tables_ = std::make_unique<FilmPrintTables>(bundle_);
  }

  static void TearDownTestSuite() {
    tables_.reset();
    bundle_.reset();
  }

  static inline std::shared_ptr<const FilmStockBundle> bundle_;
  static inline std::unique_ptr<FilmPrintTables>       tables_;
};

TEST_F(FilmPrintTablesLabTest, PrintTableMatchesLab) {
  const auto selections = PrintSelections();
  ASSERT_GE(selections.size(), 3u);
  for (const auto& selection : selections) {
    SCOPED_TRACE(selection.negative + " on " + selection.print_material + " under " +
                 selection.scene_light);
    const auto result =
        tables_->Resolve(selection.negative, selection.print_material, selection.scene_light);
    const auto file  = FilmArrayFile::Parse(ReadBytes(selection.path), selection.path);
    const auto table = file.F64("print_table", {65, 65, 65, 3});
    ASSERT_EQ(result->print_cube_root_xyz.size(), table.size());
    const auto largest = LargestRelativeDifference(result->print_cube_root_xyz, table);
    std::cout << selection.negative << " on " << selection.print_material << " under "
              << selection.scene_light << ": print table largest relative difference " << largest
              << "\n";
    EXPECT_LE(largest, kLabRelativeLimit);
    EXPECT_EQ(result->outside_curve, file.I64("outside_curve", {})[0]);
    EXPECT_EQ(result->print_not_reached, file.I64("print_not_reached", {})[0]);
  }
}

TEST_F(FilmPrintTablesLabTest, LightTableMatchesLab) {
  const auto selections = ReversalSelections();
  ASSERT_GE(selections.size(), 3u);
  for (const auto& selection : selections) {
    SCOPED_TRACE(selection.negative + " under " + selection.scene_light);
    const auto result = tables_->Resolve(selection.negative, "", selection.scene_light);
    ASSERT_TRUE(result->light_table);
    const auto file  = FilmArrayFile::Parse(ReadBytes(selection.path), selection.path);
    const auto table = file.F64("light_table", {65, 65, 65, 3});
    ASSERT_EQ(result->print_cube_root_xyz.size(), table.size());
    EXPECT_TRUE(std::ranges::all_of(result->print_cube_root_xyz,
                                    [](double value) { return std::isfinite(value); }));
    const auto largest = LargestRelativeDifference(result->print_cube_root_xyz, table);
    std::cout << "reversal " << selection.negative << " under " << selection.scene_light
              << ": light table largest relative difference " << largest << "\n";
    EXPECT_LE(largest, kLabRelativeLimit);
  }
}

TEST_F(FilmPrintTablesLabTest, ReversalSetupOfEachFilmAndLightMatchesTheLabArrays) {
  // The lab writes the setup with the films in the order of their identifiers, and the lights in
  // the order of the catalog. Each value that the product reads must be the value of the lab.
  const auto path       = fs::path(ALCEDO_FILM_PRINT_RESOURCE_DIR) / "film_print_setup.bin";
  const auto setup      = FilmArrayFile::Parse(ReadBytes(path), path);
  const auto stock_path = fs::path(ALCEDO_FILM_PRINT_RESOURCE_DIR) / "film_stock_bundle.bin";
  const auto stock      = FilmArrayFile::Parse(ReadBytes(stock_path), stock_path);
  std::vector<std::string> films;
  for (const auto& [id, entry] : stock.Metadata().at("reversal_films").items()) {
    films.push_back(id);
  }
  std::ranges::sort(films);
  const auto lights  = stock.Metadata().at("scene_lights").get<std::vector<std::string>>();
  const auto spectra = stock.Metadata().at("scene_spectra").get<std::vector<std::string>>();
  const auto r_count = static_cast<std::int64_t>(films.size());
  const auto l_count = static_cast<std::int64_t>(lights.size());
  const auto gain    = setup.F64("reversal_layer_gain", {r_count, l_count, 3});
  const auto display = setup.F64("reversal_xyz_to_display", {r_count, 3, 3});
  const auto index   = setup.I32("reversal_scene_spectrum_index", {r_count, l_count});
  ASSERT_EQ(films.size(), kFilmReversalIds.size());
  double largest = 0.0;
  int    checked = 0;
  for (std::size_t r = 0; r < films.size(); ++r) {
    for (std::size_t l = 0; l < lights.size(); ++l) {
      SCOPED_TRACE(films[r] + " under " + lights[l]);
      const auto values = bundle_->ReversalSelection(films[r], lights[l]);
      for (std::size_t k = 0; k < 3; ++k) {
        largest = (std::max)(
            largest, std::abs(values.layer_gain[k] - gain[(r * lights.size() + l) * 3 + k]));
      }
      for (std::size_t k = 0; k < 9; ++k) {
        largest = (std::max)(largest, std::abs(values.xyz_to_display[k] - display[r * 9 + k]));
      }
      const auto spectrum = stock.F64(
          "light/" + spectra[static_cast<std::size_t>(index[r * lights.size() + l])] + "/spectrum",
          {bundle_->Wavelengths()});
      for (std::size_t w = 0; w < spectrum.size(); ++w) {
        largest = (std::max)(largest, std::abs(values.scene_spectrum[w] - spectrum[w]));
      }
      // The kernel values of this selection have the same layer gain and display matrix.
      FilmKernelParams params{};
      film_print_kernel_tables::FillParams(*bundle_, films[r], "", lights[l], 65, params);
      EXPECT_EQ(params.light_table, 1U);
      EXPECT_EQ(params.layer_gain[1], static_cast<float>(values.layer_gain[1]));
      EXPECT_EQ(params.xyz_to_display[4], static_cast<float>(values.xyz_to_display[4]));
      ++checked;
    }
  }
  std::cout << "reversal setup of " << checked << " selections: largest absolute difference "
            << largest << "\n";
  EXPECT_EQ(checked, 5 * static_cast<int>(lights.size()));
  EXPECT_LE(largest, 1e-9);
}

/// The values of a scanner weights file of the lab against the weights of the product: the first
/// node and the band nodes, with 0 outside them.
auto LargestWeightsDifference(const film_negative_scan::ScannerWeights& weights,
                              const FilmArrayFile& file, const std::string& prefix,
                              std::int64_t wavelengths) -> double {
  const auto first = file.I64(prefix + "first_node", {})[0];
  const auto shape = file.Shape(prefix + "weights");
  EXPECT_EQ(shape.size(), 2U);
  EXPECT_EQ(weights.first_node, first);
  EXPECT_EQ(weights.nodes, shape[1]);
  const auto lab = file.F64(prefix + "weights", {3, shape[1]});
  double     largest = 0.0;
  for (std::int64_t k = 0; k < 3; ++k) {
    for (std::int64_t w = 0; w < wavelengths; ++w) {
      const double value = weights.weights[static_cast<std::size_t>(k * wavelengths + w)];
      const auto   band  = w - first;
      const double expected =
          band >= 0 && band < shape[1] ? lab[static_cast<std::size_t>(k * shape[1] + band)] : 0.0;
      const double scale = (std::max)(std::abs(value), std::abs(expected));
      if (scale > 0.0) largest = (std::max)(largest, std::abs(value - expected) / scale);
    }
  }
  return largest;
}

/// Largest of |a - b| over three values.
auto LargestOf3(const std::array<double, 3>& a, std::span<const double> b) -> double {
  return (std::max)({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])});
}

TEST_F(FilmPrintTablesLabTest, ScanTableMatchesLab) {
  const auto selections = ScanSelections();
  ASSERT_GE(selections.size(), 4u);
  const auto weights = film_negative_scan::MakeScannerWeights(
      film_negative_scan::DefaultScanner(bundle_->NegativeScanConstants()),
      bundle_->NegativeScanConstants(), bundle_->WavelengthsNm());
  for (const auto& selection : selections) {
    SCOPED_TRACE(selection.negative + " under " + selection.scene_light);
    const auto result =
        tables_->Resolve(selection.negative, kFilmNegativeScanId, selection.scene_light);
    ASSERT_TRUE(result->scan);
    const auto file  = FilmArrayFile::Parse(ReadBytes(selection.path), selection.path);
    const auto table = file.F64("scanner_table", {65, 65, 65, 3});
    ASSERT_EQ(result->print_cube_root_xyz.size(), table.size());
    EXPECT_TRUE(std::ranges::all_of(result->print_cube_root_xyz,
                                    [](double value) { return std::isfinite(value); }));
    const auto largest = LargestRelativeDifference(result->print_cube_root_xyz, table);
    // The scanner weights of the default scanner.
    const auto largest_weights =
        LargestWeightsDifference(weights, file, "scanner_", bundle_->Wavelengths());
    std::cout << "scan " << selection.negative << " under " << selection.scene_light
              << ": scanner table largest relative difference " << largest
              << ", scanner weights " << largest_weights << "\n";
    EXPECT_LE(largest, kLabRelativeLimit);
    EXPECT_LE(largest_weights, 1e-12);
    // The record grid of the table.
    const auto negative = bundle_->Negative(selection.negative);
    const auto start    = file.F64("density_grid_start", {3});
    const auto stop     = file.F64("density_grid_stop", {3});
    for (std::size_t k = 0; k < 3; ++k) {
      EXPECT_NEAR(negative.density_grid_start[k], start[k], 1e-12);
      EXPECT_NEAR(negative.density_grid_stop[k], stop[k], 1e-12);
    }
    // The values of the selection that the file states.
    const auto& values = result->negative_scan;
    const auto  setup  = bundle_->NegativeScanSelection(selection.negative, selection.scene_light);
    EXPECT_EQ(setup.medium, file.Metadata().at("medium").get<std::string>());
    const double largest_value =
        (std::max)({LargestOf3(values.base_density, file.F64("base_density", {3})),
                    LargestOf3(values.aim_density, file.F64("aim_density", {3})),
                    LargestOf3(values.light_axis, file.F64("light_axis", {3})),
                    LargestOf3(values.gamma, file.F64("gamma", {3})),
                    LargestOf3(setup.layer_gain, file.F64("layer_gain", {3}))});
    EXPECT_LE(largest_value, 1e-12);
    // D_base of the host from the scanner weights and the base of the negative.
    const auto base = film_negative_scan::BaseDensity(weights, negative.base);
    EXPECT_LE(LargestOf3(base, file.F64("base_density", {3})), 1e-9);
  }
}

TEST_F(FilmPrintTablesLabTest, HostDerivationOfTheDefaultScannerIsTheStoredValues) {
  // The path of a scanner other than the default (gamma of the medium, D_aim, the light axis) on
  // the default scanner gives the stored values of the lab: the host derivation is the lab form.
  const auto& constants = bundle_->NegativeScanConstants();
  const auto  scanner   = film_negative_scan::DefaultScanner(constants);
  const auto  weights =
      film_negative_scan::MakeScannerWeights(scanner, constants, bundle_->WavelengthsNm());
  std::vector<std::shared_ptr<const FilmPrintTableData>> held;
  const film_negative_scan::ScannerTableOf table_of = [&](std::string_view stock) {
    held.push_back(tables_->Resolve(std::string(stock), kFilmNegativeScanId, "reference"));
    return film_negative_scan::ScannerTableView{held.back()->print_cube_root_xyz,
                                                kFilmPrintTableEdge};
  };
  double largest_gamma = 0.0;
  for (const auto& medium : constants.media) {
    const auto gamma = film_negative_scan::DeriveMediumGamma(*bundle_, medium, weights, table_of);
    largest_gamma    = (std::max)(largest_gamma, LargestOf3(gamma, medium.gamma));
  }
  double largest = 0.0;
  for (const auto& [negative, light] :
       {std::pair<std::string, std::string>{"kodak_5207", "reference"},
        std::pair<std::string, std::string>{"kodak_portra_400", "daylight"},
        std::pair<std::string, std::string>{"kodak_5219", "tungsten"},
        std::pair<std::string, std::string>{"kodak_ektar_100", "street_mixed"}}) {
    const auto setup  = bundle_->NegativeScanSelection(negative, light);
    const auto table  = table_of(negative);
    const auto values = film_negative_scan::DeriveSelectionValues(
        *bundle_, negative, setup, weights, table,
        {setup.gamma[0], setup.gamma[1], setup.gamma[2]});
    largest = (std::max)({largest, LargestOf3(values.base_density, setup.base_density),
                          LargestOf3(values.aim_density, setup.aim_density),
                          LargestOf3(values.light_axis, setup.light_axis)});
  }
  std::cout << "host derivation on the default scanner: gamma " << largest_gamma
            << ", D_base, D_aim, and light axis " << largest << "\n";
  EXPECT_LE(largest_gamma, 1e-9);
  EXPECT_LE(largest, 1e-9);
}

TEST_F(FilmPrintTablesLabTest, ScanSetupOfEachNegativeAndLightMatchesTheLabArrays) {
  // The lab writes the setup with the negatives in the order of their identifiers, and the lights
  // in the order of the catalog. Each value that the product reads must be the value of the lab.
  const auto path       = fs::path(ALCEDO_FILM_PRINT_RESOURCE_DIR) / "film_print_setup.bin";
  const auto setup      = FilmArrayFile::Parse(ReadBytes(path), path);
  const auto stock_path = fs::path(ALCEDO_FILM_PRINT_RESOURCE_DIR) / "film_stock_bundle.bin";
  const auto stock      = FilmArrayFile::Parse(ReadBytes(stock_path), stock_path);
  std::vector<std::string> negatives;
  for (const auto& [id, entry] : stock.Metadata().at("negatives").items()) {
    negatives.push_back(id);
  }
  std::ranges::sort(negatives);
  const auto lights  = stock.Metadata().at("scene_lights").get<std::vector<std::string>>();
  const auto n_count = static_cast<std::int64_t>(negatives.size());
  const auto l_count = static_cast<std::int64_t>(lights.size());
  const auto aim     = setup.F64("negative_scan_aim_density", {n_count, l_count, 3});
  const auto axis    = setup.F64("negative_scan_light_axis", {n_count, l_count, 3});
  const auto gain    = setup.F64("layer_gain", {n_count, l_count, 3});
  double     largest = 0.0;
  int        checked = 0;
  for (std::size_t n = 0; n < negatives.size(); ++n) {
    const auto& entry  = stock.Metadata().at("negatives").at(negatives[n]);
    const auto  medium = entry.at("medium").get<std::string>();
    const auto  base   = stock.F64("negative/" + negatives[n] + "/scan_base_density", {3});
    const auto  gamma  = stock.F64("scan_medium/" + medium + "/gamma", {3});
    EXPECT_EQ(bundle_->ScanBalanceLight(negatives[n]), entry.at("scan_balance_light").get<std::string>());
    EXPECT_TRUE(bundle_->ScanBalanceLight(negatives[n]) == "D55" ||
                bundle_->ScanBalanceLight(negatives[n]) == "T3200")
        << negatives[n];
    for (std::size_t l = 0; l < lights.size(); ++l) {
      SCOPED_TRACE(negatives[n] + " under " + lights[l]);
      const auto values = bundle_->NegativeScanSelection(negatives[n], lights[l]);
      const auto at     = (n * lights.size() + l) * 3;
      EXPECT_EQ(values.medium, medium);
      largest = (std::max)({largest, LargestOf3(values.aim_density, aim.subspan(at, 3)),
                            LargestOf3(values.light_axis, axis.subspan(at, 3)),
                            LargestOf3(values.layer_gain, gain.subspan(at, 3)),
                            LargestOf3(values.base_density, base),
                            LargestOf3(values.gamma, gamma)});
      // The kernel values of this selection have the scan form and the layer gain, in float32.
      FilmKernelParams params{};
      film_print_kernel_tables::FillParams(*bundle_, negatives[n], kFilmNegativeScanId, lights[l],
                                           65, params);
      EXPECT_EQ(params.scan, 1U);
      EXPECT_EQ(params.layer_gain[1], static_cast<float>(values.layer_gain[1]));
      ++checked;
    }
  }
  // The default scanner and the media of the lab.
  const auto& constants = bundle_->NegativeScanConstants();
  EXPECT_EQ(constants.centres_nm, (std::array<double, 3>{650.0, 545.0, 450.0}));
  EXPECT_EQ(constants.width_nm, 40.0);
  EXPECT_EQ(constants.band_cut, 1e-4);
  EXPECT_EQ(constants.lamp_kelvin, 3200.0);
  EXPECT_EQ(constants.planck_c2_nm_k, 1.4388e7);
  EXPECT_EQ(constants.default_light, "halogen");
  EXPECT_EQ(constants.axis_lights[0], "T3200");
  EXPECT_EQ(constants.axis_lights[1], "D65");
  ASSERT_EQ(constants.media.size(), 2U);
  EXPECT_EQ(constants.media[0].id, "cine");
  EXPECT_EQ(constants.media[0].pool.size(), 4U);
  EXPECT_EQ(constants.media[1].id, "photo");
  EXPECT_EQ(constants.media[1].pool.size(), 6U);
  std::cout << "scan setup of " << checked << " selections: largest absolute difference "
            << largest << "\n";
  EXPECT_EQ(checked, 30 * static_cast<int>(lights.size()));
  EXPECT_LE(largest, 1e-12);
}

TEST_F(FilmPrintTablesLabTest, AsShotBlendMatchesTheLabBlend) {
  // The blend of two grid lights of the lab (scan_blend_model) against the values of the product.
  const auto blends = LabBlends();
  ASSERT_GE(blends.size(), 2u);
  for (const auto& blend : blends) {
    SCOPED_TRACE(blend.negative + " at " + std::to_string(blend.kelvin) + " K");
    const auto file = FilmArrayFile::Parse(ReadBytes(blend.path), blend.path);
    const auto mix  = bundle_->SceneLightMix(kFilmAsShotLightId, blend.kelvin);
    EXPECT_EQ(mix.first, blend.node_a);
    EXPECT_EQ(mix.second, blend.node_b);
    EXPECT_NEAR(mix.share, file.F64("blend_share", {})[0], 1e-12);
    const auto setup    = bundle_->NegativeScanSelection(blend.negative, mix);
    const auto spectrum = file.F64("spectrum", {bundle_->Wavelengths()});
    double     largest  = 0.0;
    for (std::size_t w = 0; w < spectrum.size(); ++w) {
      largest = (std::max)(largest, std::abs(setup.scene_spectrum[w] - spectrum[w]));
    }
    largest = (std::max)({largest, LargestOf3(setup.aim_density, file.F64("aim_density", {3})),
                          LargestOf3(setup.light_axis, file.F64("light_axis", {3})),
                          LargestOf3(setup.layer_gain, file.F64("layer_gain", {3}))});
    // The tables of the blend hold the same values.
    const auto result = tables_->Resolve(blend.negative, kFilmNegativeScanId, mix);
    largest = (std::max)(largest, LargestOf3(result->negative_scan.aim_density,
                                             file.F64("aim_density", {3})));
    std::cout << "As Shot " << blend.negative << " at " << blend.kelvin
              << " K: largest absolute difference " << largest << "\n";
    EXPECT_LE(largest, 1e-9);
  }
}

TEST_F(FilmPrintTablesLabTest, SecondScannerMatchesTheLab) {
  // Scanner B of the lab: other centres, another width, and an equal-energy light. The host builds
  // its weights and tables, derives the gamma of each medium again, and the values of each
  // negative.
  const auto path = fs::path(ALCEDO_FILM_PRINT_EXPECTED_DIR) / "scan__scanner_b.bin";
  if (!fs::exists(path)) GTEST_SKIP() << "No lab values of a second scanner.";
  const auto  file     = FilmArrayFile::Parse(ReadBytes(path), path);
  const auto& metadata = file.Metadata();
  const auto& lab      = metadata.at("scanner");
  const auto  centres  = lab.at("centres_nm").get<std::vector<double>>();
  const film_negative_scan::Scanner scanner{{centres[0], centres[1], centres[2]},
                                            lab.at("width_nm").get<double>(),
                                            lab.at("light").get<std::string>() == "equal_energy"};
  ASSERT_FALSE(scanner == film_negative_scan::DefaultScanner(bundle_->NegativeScanConstants()));
  const auto weights = film_negative_scan::MakeScannerWeights(
      scanner, bundle_->NegativeScanConstants(), bundle_->WavelengthsNm());
  const auto largest_weights =
      LargestWeightsDifference(weights, file, "weights_", bundle_->Wavelengths());
  EXPECT_LE(largest_weights, 1e-12);
  const auto light     = metadata.at("scene_light").get<std::string>();
  const auto negatives = metadata.at("negatives").get<std::vector<std::string>>();
  FilmPrintTables tables(bundle_);
  double          largest_gamma = 0.0;
  double          largest       = 0.0;
  for (std::size_t k = 0; k < negatives.size(); ++k) {
    SCOPED_TRACE(negatives[k]);
    const auto result = tables.ResolveNegativeScan(
        negatives[k], FilmSceneLightMix{light, light, 0.0}, scanner);
    const auto& values = result->negative_scan;
    const auto  medium = bundle_->ScanMedium(negatives[k]).id;
    largest_gamma = (std::max)(largest_gamma, LargestOf3(values.gamma, file.F64("gamma_" + medium, {3})));
    const auto index = std::to_string(k);
    largest = (std::max)({largest, LargestOf3(values.base_density, file.F64("base_density_" + index, {3})),
                          LargestOf3(values.aim_density, file.F64("aim_density_" + index, {3})),
                          LargestOf3(values.light_axis, file.F64("light_axis_" + index, {3}))});
  }
  std::cout << "scanner B: weights " << largest_weights << ", gamma " << largest_gamma
            << ", D_base, D_aim, and light axis " << largest << "; "
            << tables.Stats().scan_gamma_derivations << " derivations of gamma\n";
  EXPECT_EQ(tables.Stats().scan_gamma_derivations, 2);
  EXPECT_LE(largest_gamma, 1e-9);
  EXPECT_LE(largest, 1e-9);
}

TEST_F(FilmPrintTablesLabTest, AutoColourMatchesTheLab) {
  // Rule frame_auto_colour of the lab on two frames of x: the prescan, both estimates through rule
  // smaller_cast, and the shift of each case.
  const auto path = fs::path(ALCEDO_FILM_PRINT_EXPECTED_DIR) / "scan__auto.bin";
  if (!fs::exists(path)) GTEST_SKIP() << "No lab values of the auto colour.";
  const auto  file      = FilmArrayFile::Parse(ReadBytes(path), path);
  const auto& metadata  = file.Metadata();
  const auto& constants = metadata.at("constants");
  const auto& c         = film_negative_scan::kAutoColour;
  EXPECT_EQ(constants.at("long_edge").get<int>(), c.long_edge);
  EXPECT_EQ(constants.at("valid_log").at(0).get<double>(), c.valid_low);
  EXPECT_EQ(constants.at("valid_log").at(1).get<double>(), c.valid_high);
  EXPECT_EQ(constants.at("sigma").get<double>(), c.sigma);
  EXPECT_EQ(constants.at("iterations").get<int>(), c.iterations);
  EXPECT_EQ(constants.at("cross_share").get<double>(), c.cross_share);
  EXPECT_EQ(constants.at("highlight_share").get<double>(), c.highlight_share);
  for (std::size_t mode = 1; mode < kFilmScanAutoIds.size(); ++mode) {
    EXPECT_EQ(constants.at("target").at(std::string(kFilmScanAutoIds[mode])).get<double>(),
              c.target[mode]);
  }
  const auto axis_values = file.F64("light_axis", {3});
  const std::array<double, 3> axis{axis_values[0], axis_values[1], axis_values[2]};
  const auto cases       = metadata.at("cases");
  ASSERT_EQ(cases.size(), 5U);
  double largest_prescan = 0.0;
  double largest_shift   = 0.0;
  for (const std::string frame_name : {"a", "b"}) {
    SCOPED_TRACE(frame_name);
    const auto shape = file.Shape("frame_" + frame_name);
    ASSERT_EQ(shape.size(), 3U);
    const auto                 values = file.F64("frame_" + frame_name, {shape[0], shape[1], 3});
    film_negative_scan::Frame  frame{{values.begin(), values.end()}, shape[0], shape[1]};
    const auto                 prescan = film_negative_scan::ReduceFrame(frame);
    const auto lab_prescan = file.F64("prescan_" + frame_name, {prescan.rows, prescan.columns, 3});
    for (std::size_t i = 0; i < lab_prescan.size(); ++i) {
      largest_prescan = (std::max)(largest_prescan, std::abs(prescan.values[i] - lab_prescan[i]));
    }
    for (std::size_t index = 0; index < cases.size(); ++index) {
      const auto& entry  = cases[index];
      const auto  result = film_negative_scan::AutoColourShift(
          prescan, axis, entry.at("mode").get<std::string>(), entry.at("strength").get<double>(),
          entry.at("density").get<double>());
      largest_shift = (std::max)(largest_shift,
                                 LargestOf3(result.shift, file.F64("shift_" + frame_name + "_" +
                                                                       std::to_string(index),
                                                                   {3})));
      const bool white = std::hypot(result.white_patch_cast[0], result.white_patch_cast[1],
                                    result.white_patch_cast[2]) <
                         std::hypot(result.near_neutral_cast[0], result.near_neutral_cast[1],
                                    result.near_neutral_cast[2]);
      EXPECT_EQ(white, metadata.at("white_patch_used")
                           .at(frame_name + "_" + std::to_string(index))
                           .get<bool>())
          << index;
    }
  }
  std::cout << "auto colour: prescan largest difference " << largest_prescan << ", shift "
            << largest_shift << "\n";
  EXPECT_LE(largest_prescan, 1e-6);
  EXPECT_LE(largest_shift, 1e-6);
}

/// The display files of Negative Scan (`display/scan__*.bin`).
auto ScanDisplayFiles() -> std::vector<fs::path> {
  std::vector<fs::path> files;
  const auto directory = fs::path(ALCEDO_FILM_PRINT_EXPECTED_DIR) / "display";
  if (!fs::exists(directory)) return files;
  for (const auto& entry : fs::directory_iterator(directory)) {
    if (entry.path().filename().string().rfind("scan__", 0) == 0) files.push_back(entry.path());
  }
  std::ranges::sort(files);
  return files;
}

/// The largest |log2(a / b)| of the channels where both values are above 2^-10.
auto LargestStopsAboveFloor(std::span<const double> values, std::span<const float> lab) -> double {
  double largest = 0.0;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (values[i] > 1.0 / 1024.0 && lab[i] > 1.0 / 1024.0) {
      largest = (std::max)(largest, std::abs(std::log2(values[i] / lab[i])));
    }
  }
  return largest;
}

TEST_F(FilmPrintTablesLabTest, NegativeScanPointFormMatchesTheLab) {
  // The CPU form of the point stages of the kernels (two steps of FP5), the scanner table, rule
  // frame_auto_colour on the frame of one row, and FP6, against the lab render of the same frame:
  // a neutral ramp and the chart, with auto colour off and Neutral.
  const auto files = ScanDisplayFiles();
  ASSERT_GE(files.size(), 8u) << "Run build_display_expected.py.";
  double largest_ramp  = 0.0;
  double largest_chart = 0.0;
  for (const auto& path : files) {
    SCOPED_TRACE(path.filename().string());
    const auto  file     = FilmArrayFile::Parse(ReadBytes(path), path);
    const auto& metadata = file.Metadata();
    const auto  negative = metadata.at("negative").get<std::string>();
    const auto  mix =
        metadata.contains("kelvin")
            ? bundle_->SceneLightMix(kFilmAsShotLightId, metadata.at("kelvin").get<double>())
            : FilmSceneLightMix{metadata.at("scene_light").get<std::string>(),
                                metadata.at("scene_light").get<std::string>(), 0.0};
    const auto tables = tables_->Resolve(negative, kFilmNegativeScanId, mix);
    const film_print_reference::FilmPrintPointReference reference(*bundle_, *tables, negative,
                                                                  std::string(kFilmNegativeScanId), mix);
    const auto& values = tables->negative_scan;
    EXPECT_LE(LargestOf3(values.light_axis, file.F64("light_axis", {3})), 1e-9);
    FilmNegativeScanParams params;
    params.auto_colour   = metadata.at("auto").get<std::string>();
    const auto prescan_terms = film_negative_scan::MakePrescanTerms(values, params);
    for (const std::string part : {"ramp", "chart"}) {
      const auto count = file.Shape(part + "_ap1")[0];
      const auto ap1   = file.F32(part + "_ap1", {count, 3});
      std::vector<film_print_reference::Rgb> density(static_cast<std::size_t>(count));
      film_negative_scan::Frame               x{{}, 1, count};
      for (std::int64_t i = 0; i < count; ++i) {
        const auto at = static_cast<std::size_t>(i * 3);
        density[static_cast<std::size_t>(i)] = reference.ScanDensity(ap1[at], ap1[at + 1], ap1[at + 2]);
        const auto& t = density[static_cast<std::size_t>(i)];
        x.values.insert(x.values.end(),
                        {prescan_terms.slope[0] * t.r + prescan_terms.offset[0],
                         prescan_terms.slope[1] * t.g + prescan_terms.offset[1],
                         prescan_terms.slope[2] * t.b + prescan_terms.offset[2]});
      }
      // Rule frame_auto_colour on the frame of one row.
      const auto prescan     = film_negative_scan::ReduceFrame(x);
      const auto lab_prescan = file.F64(part + "_prescan", {prescan.rows, prescan.columns, 3});
      double     largest_x   = 0.0;
      for (std::size_t i = 0; i < lab_prescan.size(); ++i) {
        largest_x = (std::max)(largest_x, std::abs(prescan.values[i] - lab_prescan[i]));
      }
      const auto shift = film_negative_scan::AutoColourShift(prescan, values.light_axis,
                                                             params.auto_colour, params.auto_strength,
                                                             params.auto_density)
                             .shift;
      const auto lab_shift = file.F64(part + "_shift", {3});
      // The lab has the record in float32, as the kernels; this form is double, so x differs by up
      // to about 2e-6.
      EXPECT_LE(largest_x, 1e-5) << part;
      EXPECT_LE(LargestOf3(shift, lab_shift), 1e-5) << part;
      const auto terms = film_negative_scan::MakeRenderTerms(values, params, shift);
      std::vector<double> linear;
      for (const auto& t : density) {
        const auto e = film_print_reference::FilmPrintPointReference::NegativeScanLinear(t, terms);
        linear.insert(linear.end(), {e.r, e.g, e.b});
      }
      const auto stops = LargestStopsAboveFloor(linear, file.F32(part + "_linear", {count, 3}));
      (part == "ramp" ? largest_ramp : largest_chart) =
          (std::max)(part == "ramp" ? largest_ramp : largest_chart, stops);
      std::cout << path.filename().string() << " " << part << ": " << stops
                << " stops, prescan x " << largest_x << ", shift "
                << LargestOf3(shift, lab_shift) << "\n";
    }
  }
  EXPECT_LE(largest_ramp, 0.005);
  EXPECT_LE(largest_chart, 0.01);
}

TEST(FilmNegativeScanPrescanTest, PlanHasTheShapeOfTheFrameAndTheWindowsOfTheRender) {
  // A full frame of the lab: 6000 x 4006 to 96 x 64.
  const auto full = film_negative_scan::PrescanPlanOf(4006, 6000, 4006, 6000);
  EXPECT_EQ(full.columns, 96);
  EXPECT_EQ(full.rows, 64);
  ASSERT_EQ(full.windows.size(), 2U * 96U + 2U * 64U);
  std::int64_t covered = 0;
  for (std::size_t j = 0; j < 96; ++j) covered += full.windows[96 + j];
  EXPECT_EQ(covered, 6000);
  EXPECT_EQ(full.windows[0], 0);
  // A preview of the same frame names the same prescan, with the windows of its own pixels.
  const auto preview = film_negative_scan::PrescanPlanOf(4006, 6000, 1709, 2560);
  EXPECT_EQ(preview.columns, full.columns);
  EXPECT_EQ(preview.rows, full.rows);
  EXPECT_NE(preview.windows_key, full.windows_key);
  covered = 0;
  for (std::size_t j = 0; j < 96; ++j) covered += preview.windows[96 + j];
  EXPECT_EQ(covered, 2560);
  // A frame of one row, as the ramps of the lab.
  const auto row = film_negative_scan::PrescanPlanOf(1, 241, 1, 241);
  EXPECT_EQ(row.columns, 96);
  EXPECT_EQ(row.rows, 1);
  EXPECT_THROW((void)film_negative_scan::PrescanPlanOf(0, 241, 1, 241), std::invalid_argument);
}

TEST(FilmNegativeScanPrescanTest, WindowFormOfTheGpuMatchesReduceFrame) {
  // The GPU form of the prescan (issue 664): per axis the mean over the windows of PrescanAxisOf, a
  // horizontal pass and a vertical pass, each with a float32 sum and a float32 store, against
  // ReduceFrame (Pillow BOX) on random frames. Both lab frames of one row, a frame no larger than
  // the prescan, and frames with a short edge that rounds both ways.
  std::mt19937                           random(664);
  std::uniform_real_distribution<double> uniform(-2.0, 1.0);
  const std::array<std::array<std::int64_t, 2>, 7> shapes{{{1001, 1500},
                                                            {1500, 1001},
                                                            {1, 241},
                                                            {1, 4096},
                                                            {64, 48},
                                                            {97, 300},
                                                            {301, 97}}};
  for (const auto& [rows, columns] : shapes) {
    SCOPED_TRACE(std::to_string(rows) + " x " + std::to_string(columns));
    film_negative_scan::Frame frame{{}, rows, columns};
    frame.values.resize(static_cast<std::size_t>(rows * columns * 3));
    // Float32 values, as the scan density of the GPU.
    for (auto& value : frame.values) value = static_cast<float>(uniform(random));
    const auto expected = film_negative_scan::ReduceFrame(frame);
    const auto shape    = film_negative_scan::PrescanShape(rows, columns);
    ASSERT_EQ(expected.rows, shape[0]);
    ASSERT_EQ(expected.columns, shape[1]);
    const auto across = film_negative_scan::PrescanAxisOf(columns, shape[1]);
    const auto down   = film_negative_scan::PrescanAxisOf(rows, shape[0]);
    std::vector<float> middle(static_cast<std::size_t>(rows * shape[1] * 3));
    for (std::int64_t y = 0; y < rows; ++y) {
      for (std::int64_t j = 0; j < shape[1]; ++j) {
        for (std::size_t k = 0; k < 3; ++k) {
          float sum = 0.0f;
          for (std::int32_t i = 0; i < across.count[static_cast<std::size_t>(j)]; ++i) {
            const auto x = across.first[static_cast<std::size_t>(j)] + i;
            sum += static_cast<float>(frame.values[static_cast<std::size_t>((y * columns + x) * 3) + k]);
          }
          middle[static_cast<std::size_t>((y * shape[1] + j) * 3) + k] =
              sum * (1.0f / static_cast<float>(across.count[static_cast<std::size_t>(j)]));
        }
      }
    }
    double largest = 0.0;
    for (std::int64_t i = 0; i < shape[0]; ++i) {
      for (std::int64_t j = 0; j < shape[1]; ++j) {
        for (std::size_t k = 0; k < 3; ++k) {
          float sum = 0.0f;
          for (std::int32_t n = 0; n < down.count[static_cast<std::size_t>(i)]; ++n) {
            const auto y = down.first[static_cast<std::size_t>(i)] + n;
            sum += middle[static_cast<std::size_t>((y * shape[1] + j) * 3) + k];
          }
          const float value =
              sum * (1.0f / static_cast<float>(down.count[static_cast<std::size_t>(i)]));
          largest = (std::max)(
              largest, std::abs(static_cast<double>(value) -
                                expected.values[static_cast<std::size_t>((i * shape[1] + j) * 3) + k]));
        }
      }
    }
    std::cout << rows << " x " << columns << " to " << shape[0] << " x " << shape[1]
              << ": largest difference " << largest << "\n";
    // The float32 sums of up to 63 values of about 1 against the double sums of Pillow.
    EXPECT_LE(largest, 2e-6);
  }
  EXPECT_THROW((void)film_negative_scan::PrescanAxisOf(0, 1), std::invalid_argument);
}

TEST_F(FilmPrintTablesLabTest, UvTableMatchesLab) {
  const auto selections = TableSelections();
  ASSERT_GE(selections.size(), 6u);
  for (const auto& selection : selections) {
    SCOPED_TRACE(selection.negative + " under " + selection.scene_light);
    const auto result =
        tables_->Resolve(selection.negative, selection.print_material, selection.scene_light);
    const auto file     = FilmArrayFile::Parse(ReadBytes(selection.path), selection.path);
    const auto uv_nodes = result->uv_nodes;
    // The integral of the product on the compact table, against the same integral in the lab.
    const auto compact  = file.F64("uv_table_compact", {uv_nodes, uv_nodes, 3});
    ASSERT_EQ(result->uv_exposure.size(), compact.size());
    const auto largest_compact = LargestRelativeDifference(result->uv_exposure, compact);
    EXPECT_LE(largest_compact, kLabRelativeLimit);

    // The compact table against the complete table of the lab, on the nodes that a read can reach.
    const auto complete  = file.F64("uv_table", {uv_nodes, uv_nodes, 3});
    const auto reachable = file.U8("uv_reachable", {uv_nodes, uv_nodes});
    double     largest   = 0.0;
    for (std::size_t node = 0; node < reachable.size(); ++node) {
      if (reachable[node] != 0) {
        largest = (std::max)(
            largest, LargestRelativeDifference(std::span(result->uv_exposure).subspan(node * 3, 3),
                                               complete.subspan(node * 3, 3)));
      }
    }
    std::cout << selection.negative << " under " << selection.scene_light
              << ": u'v' table largest relative difference " << largest_compact
              << " against the compact lab table, " << largest << " against the complete table\n";
    EXPECT_LE(largest, kCompactTableRelativeLimit);
  }
}

TEST_F(FilmPrintTablesLabTest, ShippedReflectanceHoldsOnlyTheNodesThatAReadCanReach) {
  const auto selections = TableSelections();
  ASSERT_FALSE(selections.empty());
  const auto file      = FilmArrayFile::Parse(ReadBytes(selections[0].path), selections[0].path);
  const auto uv_nodes  = bundle_->UvNodes();
  const auto reachable = file.U8("uv_reachable", {uv_nodes, uv_nodes});
  std::vector<std::int32_t> expected;
  for (std::size_t node = 0; node < reachable.size(); ++node) {
    if (reachable[node] != 0) {
      expected.push_back(static_cast<std::int32_t>(node));
    }
  }
  const auto nodes = bundle_->ReflectanceNodes();
  EXPECT_TRUE(std::equal(nodes.begin(), nodes.end(), expected.begin(), expected.end()));
  EXPECT_LT(nodes.size(), reachable.size());
  EXPECT_LT(bundle_->ReflectanceWavelengths(), bundle_->Wavelengths());
}

TEST_F(FilmPrintTablesLabTest, ShippedListsHoldTheStocksOfThePlan) {
  EXPECT_EQ(bundle_->Negatives().size(), 30u);
  EXPECT_EQ(bundle_->PrintMaterials().size(), 19u);
  EXPECT_EQ(bundle_->Reversals().size(), 5u);
  // 11 named lights and the colour temperature grid of As Shot: 5 mired from 500 to 70, and
  // 15000 K.
  EXPECT_EQ(bundle_->SceneLights().size(), 11u + 88u);
  EXPECT_EQ(bundle_->CctGrid().lights.size(), 88u);
  EXPECT_EQ(bundle_->CctGrid().kelvin.front(), 2000.0);
  EXPECT_EQ(bundle_->CctGrid().kelvin.back(), 15000.0);
  EXPECT_EQ(bundle_->SceneLights()[0], "reference");
  EXPECT_NO_THROW((void)bundle_->Selection("kodak_5207", "kodak_2383", "reference"));
}

TEST_F(FilmPrintTablesLabTest, OnlyCineLookHasTheCinemaLook) {
  for (const auto& material : bundle_->PrintMaterials()) {
    EXPECT_EQ(bundle_->PrintMaterial(material.id).cinema_look, material.id == "cine_look")
        << material.id;
  }
}

TEST_F(FilmPrintTablesLabTest, OnlyTungstenStocksHaveTheNegativeGradeWithItsGrey) {
  constexpr std::array<std::string_view, 7> tungsten = {
      "kodak_5219",     "kodak_5213",      "kodak_5277",           "kodak_5293",
      "kodak_exr_5248", "fuji_eterna_500", "fuji_eterna_500_vivid"};
  for (const auto& negative : bundle_->Negatives()) {
    const bool expected = IsFilmIdentifier(tungsten, negative.id);
    EXPECT_EQ(bundle_->Negative(negative.id).negative_grade, expected) << negative.id;
    const auto grey = bundle_->Selection(negative.id, "cine_look", "reference").negative_grade_grey;
    EXPECT_EQ(grey[1] > 0.0, expected) << negative.id;
  }
}

TEST_F(FilmPrintTablesLabTest, ShippedListsMatchTheIdentifiersOfTheDocumentModel) {
  ASSERT_EQ(bundle_->Negatives().size(), kFilmNegativeIds.size());
  for (const auto& negative : bundle_->Negatives()) {
    EXPECT_TRUE(IsFilmIdentifier(kFilmNegativeIds, negative.id)) << negative.id;
  }
  // The document list has Negative Scan first, and then the print materials of the bundle.
  ASSERT_EQ(bundle_->PrintMaterials().size() + 1, kFilmPrintMaterialIds.size());
  EXPECT_EQ(kFilmPrintMaterialIds[0], kFilmNegativeScanId);
  EXPECT_EQ(bundle_->NegativeScan().id, kFilmNegativeScanId);
  for (const auto& material : bundle_->PrintMaterials()) {
    EXPECT_TRUE(IsFilmIdentifier(kFilmPrintMaterialIds, material.id)) << material.id;
    EXPECT_NE(material.id, kFilmNegativeScanId);
  }
  // The document lights are the named lights of the bundle and As Shot, which the host resolves
  // on the grid of the bundle.
  std::vector<std::string> named;
  for (const auto& light : bundle_->SceneLights()) {
    if (std::ranges::find(bundle_->CctGrid().lights, light) == bundle_->CctGrid().lights.end()) {
      named.push_back(light);
    }
  }
  ASSERT_EQ(named.size() + 1, kFilmSceneLightIds.size());
  for (const auto& light : named) {
    EXPECT_TRUE(IsFilmIdentifier(kFilmSceneLightIds, light)) << light;
  }
  EXPECT_TRUE(IsFilmIdentifier(kFilmSceneLightIds, kFilmAsShotLightId));
  EXPECT_EQ(std::ranges::find(bundle_->SceneLights(), std::string(kFilmAsShotLightId)),
            bundle_->SceneLights().end());
  // The editor list has the reversal films in the order of the lab list.
  ASSERT_EQ(bundle_->ReversalListOrder().size(), kFilmReversalIds.size());
  for (std::size_t index = 0; index < kFilmReversalIds.size(); ++index) {
    EXPECT_EQ(bundle_->ReversalListOrder()[index], kFilmReversalIds[index]);
    EXPECT_TRUE(bundle_->IsReversal(kFilmReversalIds[index]));
    EXPECT_FALSE(IsFilmIdentifier(kFilmNegativeIds, kFilmReversalIds[index]));
  }
}

TEST_F(FilmPrintTablesLabTest, ShippedNamesMatchTheNamesOfTheDocumentModel) {
  const auto expect_names = [](const auto& shipped, const auto& ids, const auto& names) {
    for (const auto& entry : shipped) {
      const auto position = std::find(ids.begin(), ids.end(), entry.id);
      ASSERT_NE(position, ids.end()) << entry.id;
      EXPECT_EQ(entry.display_name, names[static_cast<std::size_t>(position - ids.begin())]);
    }
  };
  expect_names(bundle_->Negatives(), kFilmNegativeIds, kFilmNegativeNames);
  expect_names(bundle_->PrintMaterials(), kFilmPrintMaterialIds, kFilmPrintMaterialNames);
  EXPECT_EQ(bundle_->NegativeScan().display_name, kFilmPrintMaterialNames[0]);
  expect_names(bundle_->Reversals(), kFilmReversalIds, kFilmReversalNames);
}

TEST_F(FilmPrintTablesLabTest, ReversalFilmsWithNoTransferDataHaveNoSpreadOrInhibition) {
  // Astia 100F has no modulation transfer curve, and the chart of E100 has no rise above 1.
  const auto astia = bundle_->Reversal("fuji_astia_100f").record;
  EXPECT_TRUE(std::ranges::all_of(astia.layer_spread_um, [](double v) { return v == 0.0; }));
  EXPECT_FALSE(astia.inhibition.Present());
  const auto e100 = bundle_->Reversal("kodak_ektachrome_e100").record;
  EXPECT_TRUE(std::ranges::all_of(e100.layer_spread_um, [](double v) { return v > 0.0; }));
  FilmKernelParams params{};
  film_print_kernel_tables::FillParams(*bundle_, "kodak_ektachrome_e100", "", "reference", 65,
                                       params);
  EXPECT_EQ(params.inhibition_has_image, 0U);
  EXPECT_EQ(params.inhibition_has_point, 0U);
  EXPECT_TRUE(bundle_->Reversal("fuji_velvia_50").record.inhibition.Present());
}

TEST_F(FilmPrintTablesLabTest, RecordsTheHostTimeOfEachStage) {
  const auto opened = std::chrono::steady_clock::now();
  const auto bundle = FilmStockBundle::Open(fs::path(ALCEDO_FILM_PRINT_RESOURCE_DIR));
  std::cout << "Open: "
            << std::chrono::duration<double>(std::chrono::steady_clock::now() - opened).count()
            << " s\n";
  FilmPrintTables tables(bundle);
  (void)tables.Resolve("kodak_5219", "fuji_ca_maxima", "tungsten");
  const auto first = tables.Stats();
  std::cout << "u'v' table: " << first.last_uv_seconds
            << " s; stage A: " << first.last_negative_seconds
            << " s; stage B: " << first.last_paper_seconds
            << " s; composition: " << first.last_compose_seconds << " s\n";
  EXPECT_GT(first.last_uv_seconds, 0.0);
  EXPECT_GT(first.last_negative_seconds, 0.0);
  EXPECT_GT(first.last_paper_seconds, 0.0);
  EXPECT_GT(first.last_compose_seconds, 0.0);
  (void)tables.Resolve("fuji_velvia_50", "", "reference");
  const auto reversal = tables.Stats();
  std::cout << "reversal u'v' table: " << reversal.last_uv_seconds
            << " s; light table: " << reversal.last_light_table_seconds << " s\n";
  EXPECT_GT(reversal.last_light_table_seconds, 0.0);
  (void)tables.Resolve("kodak_5207", kFilmNegativeScanId, "reference");
  const auto scan = tables.Stats();
  std::cout << "scan u'v' table: " << scan.last_uv_seconds
            << " s; scanner table: " << scan.last_scan_table_seconds << " s\n";
  EXPECT_GT(scan.last_scan_table_seconds, 0.0);
}

#else

TEST(FilmPrintTablesLabTest, LabValuesAreConfigured) {
  GTEST_SKIP() << "ALCEDO_FILM_PRINT_RESOURCE_DIR and ALCEDO_FILM_PRINT_EXPECTED_DIR are not "
                  "configured, so the comparison with the lab did not run.";
}

#endif

}  // namespace
}  // namespace alcedo
