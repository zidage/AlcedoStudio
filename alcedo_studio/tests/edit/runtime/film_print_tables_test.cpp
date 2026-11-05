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
#include "edit/runtime/film/film_print_kernel_tables.hpp"
#include "edit/runtime/film/film_print_math.h"
#include "edit/runtime/film/film_stock_bundle.hpp"
#include "edit/runtime/film/film_tables.hpp"
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

/// Small resources with two negatives, two papers, and two scene lights. The values have no
/// physical meaning. Each one is distinct, so that a wrong index gives a wrong table.
void WriteSyntheticResources(const fs::path& directory, double surround_exponent = 1.0,
                             const std::string& lab_scan_id      = "lab_scan",
                             double             first_scan_scale = 0.91,
                             const std::string& printing_paper   = "paper_a") {
  fs::create_directories(directory);
  std::map<std::string, ArrayValues> stock;
  stock["common/wavelengths_nm"] = F64Array({kWavelengths}, Flat(500.0));
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
  // The ADX16 encoding and the constants of the Academy ADX to ACES transform (aces-dev v1.3.1).
  stock["common/adx_code_per_density"] = F64Array({}, {8000.0});
  stock["common/adx_code_base"]        = F64Array({}, {1520.0});
  stock["common/adx_lad_aim"]          = F64Array({}, {7120.0});
  stock["common/adx_code_max"]         = F64Array({}, {65535.0});
  stock["common/adx_equation_scale"]   = F64Array({3}, {1.00, 0.92, 0.95});
  stock["common/adx_cdd_to_cid"]       = F64Array(
      {3, 3}, {0.75573, 0.05901, 0.16134, 0.22197, 0.96928, 0.07406, 0.02230, -0.02829, 0.76460});
  stock["common/adx_exp_to_aces"] = F64Array(
      {3, 3}, {0.72286, 0.11923, 0.01427, 0.12630, 0.76418, 0.08213, 0.15084, 0.11659, 0.90359});
  stock["common/adx_lut_cid"] =
      F64Array({11}, {-0.19, 0.01, 0.028, 0.054, 0.095, 0.145, 0.22, 0.3, 0.4, 0.5, 0.6});
  stock["common/adx_lut_log_e"] =
      F64Array({11}, {-6.0, -2.721718645, -2.521718645, -2.321718645, -2.121718645, -1.921718645,
                      -1.721718645, -1.521718645, -1.321718645, -1.121718645, -0.926545676714876});
  stock["common/adx_ref_pt"] =
      F64Array({}, {(7120.0 - 1520.0) / 8000.0 * (100.0 / 55.0) - std::log10(0.18)});
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
    stock[name("adx_base_printing_density")] =
        F64Array({3}, {0.2 * scale, 0.3 * scale, 0.4 * scale});
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
  const nlohmann::json metadata{
      {"negatives", {{"neg_a", {{"name", "Negative A"}}}, {"neg_b", {{"name", "Negative B"}}}}},
      {"print_materials", {{"paper_a", {{"name", "Paper A"}}}, {"paper_b", {{"name", "Paper B"}}}}},
      {"reversal_films", {{"rev_a", {{"name", "Reversal A"}}}}},
      {"reversal_list_order", {"rev_a"}},
      {"scene_lights", {"reference", "daylight"}},
      {"scene_spectra", {"flat", "ramp"}},
      {"lab_scan", {{"id", lab_scan_id}, {"name", "Lab Scan"}, {"printing_paper", printing_paper}}},
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
  // Lab Scan, per negative and scene light.
  setup["adx_channel_scale"] = F64Array({2, 2, 3}, {first_scan_scale, 0.92, 0.93, 0.94, 0.92, 0.96,
                                                    0.97, 0.92, 0.99, 0.81, 0.92, 0.83});
  setup["adx_grey_printing_density"] =
      F64Array({2, 2, 3}, {0.9, 1.0, 1.1, 0.95, 1.05, 1.15, 0.8, 0.85, 0.9, 0.82, 0.87, 0.92});
  setup["adx_grey"] = F64Array({2, 2, 3}, {6000.0, 6100.0, 6200.0, 6300.0, 6400.0, 6500.0, 5000.0,
                                           5100.0, 5200.0, 5300.0, 5400.0, 5500.0});
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

TEST_F(FilmPrintTablesTest, ScanSelectionReadsTheValuesOfItsNegativeAndLight) {
  const auto bundle = FilmStockBundle::Open(Resources());
  EXPECT_EQ(bundle->LabScan().id, "lab_scan");
  EXPECT_EQ(bundle->LabScan().display_name, "Lab Scan");
  EXPECT_TRUE(bundle->IsLabScan("lab_scan"));
  EXPECT_FALSE(bundle->IsLabScan("paper_a"));
  // Lab Scan is not a print material of the bundle.
  EXPECT_THROW((void)bundle->PrintMaterial("lab_scan"), std::runtime_error);

  // neg_b is the second negative and daylight the second light.
  const auto scan = bundle->ScanSelection("neg_b", "daylight");
  EXPECT_EQ(scan.channel_scale, (std::array<double, 3>{0.81, 0.92, 0.83}));
  EXPECT_EQ(scan.grey_printing_density, (std::array<double, 3>{0.82, 0.87, 0.92}));
  EXPECT_EQ(scan.grey_code, (std::array<double, 3>{5300.0, 5400.0, 5500.0}));
  EXPECT_EQ(scan.base_printing_density, (std::array<double, 3>{0.2 * 1.2, 0.3 * 1.2, 0.4 * 1.2}));
  // The layer gain and the scene spectrum are those of a print of the same negative and light.
  const auto print = bundle->Selection("neg_b", "paper_a", "daylight");
  EXPECT_EQ(scan.layer_gain, print.layer_gain);
  EXPECT_EQ(scan.scene_spectrum, print.scene_spectrum);
  // The printing weights are those of the printing paper of the metadata.
  const auto& constants = bundle->ScanConstants();
  EXPECT_EQ(constants.printing_paper, "paper_a");
  EXPECT_EQ(constants.printing_weights.data(),
            bundle->PrintMaterial("paper_a").printing_weights.data());
  EXPECT_EQ(constants.code_per_density, 8000.0);
  EXPECT_EQ(constants.code_base, 1520.0);
  EXPECT_EQ(constants.lad_aim, 7120.0);
  EXPECT_EQ(constants.code_max, 65535.0);
  EXPECT_EQ(constants.cdd_to_cid[3], 0.22197);
  EXPECT_EQ(constants.exp_to_aces[8], 0.90359);
  EXPECT_EQ(constants.lut_cid.size(), 11U);
  EXPECT_EQ(constants.lut_log_e[10], -0.926545676714876);

  EXPECT_THROW((void)bundle->ScanSelection("rev_a", "daylight"), std::runtime_error);
  EXPECT_THROW((void)bundle->ScanSelection("neg_a", "no_light"), std::runtime_error);
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
  const auto sa  = bundle->ScanSelection("neg_a", "reference");
  const auto sb  = bundle->ScanSelection("neg_a", "daylight");
  const auto sab = bundle->ScanSelection("neg_a", mix);
  for (std::size_t k = 0; k < 3; ++k) {
    EXPECT_NEAR(sab.channel_scale[k], 0.75 * sa.channel_scale[k] + 0.25 * sb.channel_scale[k],
                1e-15);
    EXPECT_NEAR(sab.grey_printing_density[k],
                0.75 * sa.grey_printing_density[k] + 0.25 * sb.grey_printing_density[k], 1e-15);
    EXPECT_NEAR(sab.grey_code[k], 0.75 * sa.grey_code[k] + 0.25 * sb.grey_code[k], 1e-12);
    EXPECT_EQ(sab.base_printing_density[k], sa.base_printing_density[k]);
  }
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
  // The Lab Scan of the blend shares the u'v' table of the print of the blend.
  const auto scan = tables.Resolve("neg_a", "lab_scan", mix);
  EXPECT_EQ(scan->uv_key, blend->uv_key);
  EXPECT_EQ(scan->print_key, tables.Resolve("neg_a", "lab_scan", "reference")->print_key);
}

TEST_F(FilmPrintTablesTest, ScanTableIsThePrintingDensityOfTheNegative) {
  const auto      bundle = FilmStockBundle::Open(Resources());
  FilmPrintTables tables(bundle);
  const auto      result = tables.Resolve("neg_a", "lab_scan", "reference");
  ASSERT_TRUE(result->scan);
  EXPECT_FALSE(result->light_table);
  const auto     negative = bundle->Negative("neg_a");
  const auto     weights  = bundle->PrintMaterial("paper_a").printing_weights;
  constexpr auto kEdge    = static_cast<std::size_t>(kFilmPrintTableEdge);
  ASSERT_EQ(result->print_cube_root_xyz.size(), kEdge * kEdge * kEdge * 3);
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
      double sum    = 0.0;
      double weight = 0.0;
      for (std::int64_t w = 0; w < kWavelengths; ++w) {
        const auto   index   = static_cast<std::size_t>(w);
        const double density = negative.base[index] + amounts[0] * negative.dyes[index] +
                               amounts[1] * negative.dyes[kWavelengths + index] +
                               amounts[2] * negative.dyes[2 * kWavelengths + index];
        sum += std::pow(10.0, -density) * weights[k * kWavelengths + index];
        weight += weights[k * kWavelengths + index];
      }
      const auto value = result->print_cube_root_xyz[((r * kEdge + g) * kEdge + b) * 3 + k];
      EXPECT_NEAR(value, std::log10(weight) - std::log10(sum), 1e-12);
    }
  }
  // The GPU form has the same values, with red varying fastest.
  const auto* rgba = reinterpret_cast<const float*>(result->print_rgba.data());
  EXPECT_FLOAT_EQ(rgba[((12 * kEdge + 0) * kEdge + 64) * 4 + 1],
                  static_cast<float>(result->print_cube_root_xyz[((64 * kEdge + 0) * kEdge + 12) * 3 + 1]));
}

TEST_F(FilmPrintTablesTest, ScanReadsNoPaperAndSharesTheTablesOfTheNegativeWithAPrint) {
  FilmPrintTables tables(FilmStockBundle::Open(Resources()));
  const auto      scan  = tables.Resolve("neg_a", "lab_scan", "reference");
  const auto      print = tables.Resolve("neg_a", "paper_a", "reference");
  // A change between Lab Scan and a print material keeps the GPU copies of the u'v' table and of
  // the kernel values, which have the same content.
  EXPECT_EQ(scan->uv_key, print->uv_key);
  EXPECT_EQ(scan->kernel_key, print->kernel_key);
  EXPECT_EQ(scan->uv_exposure, print->uv_exposure);
  EXPECT_EQ(scan->kernel_values, print->kernel_values);
  EXPECT_NE(scan->print_key, print->print_key);
  // The printing density table depends on the negative only.
  const auto daylight = tables.Resolve("neg_a", "lab_scan", "daylight");
  EXPECT_NE(daylight.get(), scan.get());
  EXPECT_EQ(daylight->print_key, scan->print_key);
  EXPECT_NE(tables.Resolve("neg_b", "lab_scan", "reference")->print_key, scan->print_key);
  EXPECT_EQ(tables.Resolve("neg_a", "lab_scan", "reference").get(), scan.get());
  const auto stats = tables.Stats();
  EXPECT_EQ(stats.scan_table_builds, 2);
  EXPECT_EQ(stats.compose_builds, 1);
  EXPECT_EQ(stats.light_table_builds, 0);
  // A colour reversal film reads no print material, so a stored Lab Scan gives the light table.
  const auto reversal = tables.Resolve("rev_a", "lab_scan", "reference");
  EXPECT_TRUE(reversal->light_table);
  EXPECT_FALSE(reversal->scan);
  EXPECT_EQ(reversal.get(), tables.Resolve("rev_a", "paper_a", "reference").get());
}

TEST_F(FilmPrintTablesTest, ScanKernelParamsHaveTheScanForm) {
  const auto       bundle = FilmStockBundle::Open(Resources());
  const auto       light  = FilmSceneLightMix{"daylight", "daylight", 0.0};
  FilmKernelParams params{};
  film_print_kernel_tables::FillParams(*bundle, "neg_b", "lab_scan", light, false, 65, params);
  EXPECT_EQ(params.scan, 1U);
  EXPECT_EQ(params.light_table, 0U);
  EXPECT_EQ(params.cinema_look, 0U);
  EXPECT_EQ(params.negative_grade, 0U);
  EXPECT_EQ(params.display_surround_power, 0.0f);
  EXPECT_FLOAT_EQ(params.scan_grey_density[0], 0.82f);
  // With the LAD aim off, the code of the grey has no offset: the scan is untimed.
  EXPECT_FLOAT_EQ(params.scan_grey_code[1], 5400.0f);
  EXPECT_FLOAT_EQ(params.scan_code_slope[2], static_cast<float>(8000.0 * 0.83));
  EXPECT_FLOAT_EQ(params.scan_code_base, 1520.0f);
  EXPECT_FLOAT_EQ(params.scan_code_max, 65535.0f);
  EXPECT_FLOAT_EQ(params.scan_code_per_density, 8000.0f);
  EXPECT_FLOAT_EQ(params.scan_cdd_to_cid[1], 0.05901f);
  EXPECT_FLOAT_EQ(params.scan_lut_cid[10], 0.6f);
  EXPECT_FLOAT_EQ(params.scan_lut_log_e[0], -6.0f);
  EXPECT_FLOAT_EQ(params.layer_gain[0], 1.9f);
  // EXP_TO_ACES (row vectors), then ACES2065-1 to AP1: AP0 (1, 0, 0) is AP1 (1.4514, -0.0766,
  // 0.0083), so the first row of a unit exposure (1, 0, 0) is EXP_TO_ACES row 0 in AP1.
  const double ap0_to_ap1[9] = {1.4514393161,  -0.2365107469, -0.2149285693,
                                -0.0765537734, 1.1762296998,  -0.0996759264,
                                0.0083161484,  -0.0060324498, 0.9977163014};
  const double exp_row0[3]   = {0.72286, 0.11923, 0.01427};
  for (std::size_t j = 0; j < 3; ++j) {
    double expected = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
      expected += exp_row0[i] * ap0_to_ap1[j * 3 + i];
    }
    EXPECT_NEAR(params.scan_exp_to_ap1[j], expected, 2e-6) << j;
  }
  // With the LAD aim on, the grey goes to the aim on each channel.
  FilmKernelParams timed{};
  film_print_kernel_tables::FillParams(*bundle, "neg_b", "lab_scan", light, true, 65, timed);
  EXPECT_EQ(timed.scan_grey_code[0], 7120.0f);
  EXPECT_EQ(timed.scan_grey_code[2], 7120.0f);
  // The record grid of the printing density table is the grid of the print table.
  const auto negative = bundle->Negative("neg_b");
  EXPECT_FLOAT_EQ(params.print_step[1],
                  static_cast<float>((negative.density_grid_stop[1] - negative.density_grid_start[1]) / 64.0));

  // The other forms are not the scan form, and the LAD aim does not change them.
  FilmKernelParams print{};
  film_print_kernel_tables::FillParams(*bundle, "neg_b", "paper_a", light, true, 65, print);
  EXPECT_EQ(print.scan, 0U);
  FilmKernelParams print_off{};
  film_print_kernel_tables::FillParams(*bundle, "neg_b", "paper_a", "daylight", 65, print_off);
  EXPECT_EQ(std::memcmp(&print, &print_off, sizeof(print)), 0);
  FilmKernelParams reversal{};
  film_print_kernel_tables::FillParams(*bundle, "rev_a", "lab_scan", "daylight", 65, reversal);
  EXPECT_EQ(reversal.scan, 0U);
  EXPECT_EQ(reversal.light_table, 1U);
}

/// The scan form of issue 661 in double precision, from the stored values: the lab
/// `scan_point_form` of build_product_resources.py after the read of the table.
auto ScanAcesOfDensity(const FilmStockBundle& bundle, const FilmScanSetup& setup, bool lad_aim,
                       std::array<double, 3> density) -> std::array<double, 3> {
  const auto&           scan = bundle.ScanConstants();
  std::array<double, 3> cdd{};
  for (std::size_t k = 0; k < 3; ++k) {
    const double offset = lad_aim ? scan.lad_aim - setup.grey_code[k] : 0.0;
    const double code   = std::clamp(setup.grey_code[k] +
                                         scan.code_per_density * setup.channel_scale[k] *
                                             (density[k] - setup.grey_printing_density[k]) +
                                         offset,
                                     0.0, scan.code_max);
    cdd[k]              = (code - scan.code_base) / scan.code_per_density;
  }
  std::array<double, 3> exposure{};
  for (std::size_t j = 0; j < 3; ++j) {
    const double cid = cdd[0] * scan.cdd_to_cid[j] + cdd[1] * scan.cdd_to_cid[3 + j] +
                       cdd[2] * scan.cdd_to_cid[6 + j];
    double log_e = 0.0;
    if (cid > 0.6) {
      log_e = (100.0 / 55.0) * cid - scan.ref_pt;
    } else if (cid <= scan.lut_cid[0]) {
      log_e = scan.lut_log_e[0];
    } else {
      std::size_t k = 1;
      while (cid > scan.lut_cid[k]) ++k;
      const double t = (cid - scan.lut_cid[k - 1]) / (scan.lut_cid[k] - scan.lut_cid[k - 1]);
      log_e          = scan.lut_log_e[k - 1] + t * (scan.lut_log_e[k] - scan.lut_log_e[k - 1]);
    }
    exposure[j] = std::pow(10.0, log_e);
  }
  std::array<double, 3> aces{};
  for (std::size_t j = 0; j < 3; ++j) {
    aces[j] = exposure[0] * scan.exp_to_aces[j] + exposure[1] * scan.exp_to_aces[3 + j] +
              exposure[2] * scan.exp_to_aces[6 + j];
  }
  return aces;
}

TEST_F(FilmPrintTablesTest, ScanSceneIsTheAcademyTransformOfTheAdx16Code) {
  const auto      bundle = FilmStockBundle::Open(Resources());
  FilmPrintTables tables(bundle);
  const auto      data          = tables.Resolve("neg_a", "lab_scan", "reference");
  const auto      setup         = bundle->ScanSelection("neg_a", "reference");
  const auto*     rgba          = reinterpret_cast<const float*>(data->print_rgba.data());
  const auto      negative      = bundle->Negative("neg_a");
  // AP1 to ACES2065-1, for the comparison in ACES.
  const double    ap1_to_ap0[9] = {0.6954522414,  0.1406786965, 0.1638690622,
                                   0.0447945634,  0.8596711185, 0.0955343182,
                                   -0.0055258826, 0.0040252103, 1.0015006723};
  for (const bool lad : {false, true}) {
    FilmKernelParams params{};
    film_print_kernel_tables::FillParams(*bundle, "neg_a", "lab_scan",
                                         FilmSceneLightMix{"reference", "reference", 0.0}, lad,
                                         data->edge, params);
    for (const auto& node :
         {std::array<std::size_t, 3>{0, 0, 0}, std::array<std::size_t, 3>{20, 33, 41},
          std::array<std::size_t, 3>{64, 64, 64}, std::array<std::size_t, 3>{50, 5, 30}}) {
      SCOPED_TRACE(std::to_string(node[0]) + ", " + std::to_string(node[1]) + ", " +
                   std::to_string(node[2]) + (lad ? ", LAD aim on" : ", LAD aim off"));
      // A record on a node reads the node value with no interpolation.
      std::array<float, 3>  record{};
      std::array<double, 3> density{};
      for (std::size_t k = 0; k < 3; ++k) {
        record[k]  = params.print_start[k] + params.print_step[k] * static_cast<float>(node[k]);
        density[k] = data->print_cube_root_xyz[((node[0] * 65 + node[1]) * 65 + node[2]) * 3 + k];
      }
      const auto ap1 =
          FilmKernelScanSceneAp1(&params, rgba, FilmKernelMakeRgb(record[0], record[1], record[2]));
      const std::array<double, 3> got{ap1.r, ap1.g, ap1.b};
      const auto                  expected = ScanAcesOfDensity(*bundle, setup, lad, density);
      for (std::size_t j = 0; j < 3; ++j) {
        const double aces = ap1_to_ap0[j * 3] * got[0] + ap1_to_ap0[j * 3 + 1] * got[1] +
                            ap1_to_ap0[j * 3 + 2] * got[2];
        EXPECT_NEAR(aces, expected[j], 2e-5 * (std::max)(std::abs(expected[j]), 1e-3)) << j;
      }
    }
  }
  (void)negative;
}

TEST_F(FilmPrintTablesTest, ScanLogExposureFollowsTheAcademyTable) {
  const auto       bundle = FilmStockBundle::Open(Resources());
  FilmKernelParams params{};
  film_print_kernel_tables::FillParams(*bundle, "neg_a", "lab_scan", "reference", 65, params);
  // Below the first node: the end value, as numpy.interp.
  EXPECT_EQ(FilmKernelScanLogExposure(&params, -1.0f), -6.0f);
  EXPECT_EQ(FilmKernelScanLogExposure(&params, -0.19f), -6.0f);
  // On a node: the node value. Between: the straight line.
  EXPECT_NEAR(FilmKernelScanLogExposure(&params, 0.3f), -1.521718645f, 1e-6f);
  EXPECT_NEAR(FilmKernelScanLogExposure(&params, 0.35f), -1.421718645f, 1e-6f);
  // Above 0.6: slope 100 / 55, and no step at 0.6.
  const float ref_pt = params.scan_ref_pt;
  EXPECT_NEAR(FilmKernelScanLogExposure(&params, 0.6f), -0.926545676714876f, 1e-6f);
  EXPECT_NEAR(FilmKernelScanLogExposure(&params, 0.6001f), 100.0f / 55.0f * 0.6001f - ref_pt,
              1e-6f);
  EXPECT_NEAR(100.0f / 55.0f * 0.6f - ref_pt, -0.926545676714876f, 2e-6f);
  EXPECT_NEAR(FilmKernelScanLogExposure(&params, 1.0f), 100.0f / 55.0f - ref_pt, 1e-6f);
}

TEST_F(FilmPrintTablesTest, LabScanThatNamesAPrintMaterialThrows) {
  WriteSyntheticResources(root_ / "lab_scan_names_a_paper", 1.0, "paper_a");
  const auto error = OpenError(root_ / "lab_scan_names_a_paper");
  EXPECT_NE(error.find("Lab Scan"), std::string::npos) << error;
  EXPECT_NE(error.find("paper_a"), std::string::npos) << error;
}

TEST_F(FilmPrintTablesTest, LabScanPrintingPaperThatIsNotAPrintMaterialThrows) {
  WriteSyntheticResources(root_ / "lab_scan_no_paper", 1.0, "lab_scan", 0.91, "paper_z");
  const auto error = OpenError(root_ / "lab_scan_no_paper");
  EXPECT_NE(error.find("printing paper"), std::string::npos) << error;
  EXPECT_NE(error.find("paper_z"), std::string::npos) << error;
}

TEST_F(FilmPrintTablesTest, ScanChannelScaleOfZeroThrows) {
  // Rule 'adx_slope_match' divides by the slope of each channel.
  WriteSyntheticResources(root_ / "scan_scale_zero", 1.0, "lab_scan", 0.0);
  const auto error = OpenError(root_ / "scan_scale_zero");
  EXPECT_NE(error.find("channel scale"), std::string::npos) << error;
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
  /// A file of print material Lab Scan holds the printing density table and the scan values, and
  /// no u'v' table.
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
    // A blend file has no table: see LabBlends.
    if (metadata.contains("kelvin")) continue;
    if (metadata.contains("reversal")) {
      selections.push_back({entry.path(), metadata.at("reversal").get<std::string>(), "",
                            metadata.at("scene_light").get<std::string>(), true});
      continue;
    }
    const auto material = metadata.at("print_material").get<std::string>();
    selections.push_back({entry.path(), metadata.at("negative").get<std::string>(), material,
                          metadata.at("scene_light").get<std::string>(), false,
                          material == kFilmLabScanId});
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

TEST_F(FilmPrintTablesLabTest, ScanTableMatchesLab) {
  const auto selections = ScanSelections();
  ASSERT_GE(selections.size(), 4u);
  for (const auto& selection : selections) {
    SCOPED_TRACE(selection.negative + " under " + selection.scene_light);
    const auto result = tables_->Resolve(selection.negative, kFilmLabScanId, selection.scene_light);
    ASSERT_TRUE(result->scan);
    const auto file  = FilmArrayFile::Parse(ReadBytes(selection.path), selection.path);
    const auto table = file.F64("printing_density_table", {65, 65, 65, 3});
    ASSERT_EQ(result->print_cube_root_xyz.size(), table.size());
    EXPECT_TRUE(std::ranges::all_of(result->print_cube_root_xyz,
                                    [](double value) { return std::isfinite(value); }));
    const auto largest = LargestRelativeDifference(result->print_cube_root_xyz, table);
    std::cout << "scan " << selection.negative << " under " << selection.scene_light
              << ": printing density table largest relative difference " << largest << "\n";
    EXPECT_LE(largest, kLabRelativeLimit);
    // The record grid of the table.
    const auto negative = bundle_->Negative(selection.negative);
    const auto start    = file.F64("density_grid_start", {3});
    const auto stop     = file.F64("density_grid_stop", {3});
    for (std::size_t k = 0; k < 3; ++k) {
      EXPECT_NEAR(negative.density_grid_start[k], start[k], 1e-12);
      EXPECT_NEAR(negative.density_grid_stop[k], stop[k], 1e-12);
    }
    // The values of the selection that the file states.
    const auto setup         = bundle_->ScanSelection(selection.negative, selection.scene_light);
    const auto base          = file.F64("adx_base_printing_density", {3});
    const auto pivot         = file.F64("adx_grey_printing_density", {3});
    const auto scale         = file.F64("adx_channel_scale", {3});
    const auto grey          = file.F64("adx_grey", {3});
    const auto lad_offset    = file.F64("lad_offset", {3});
    double     largest_value = 0.0;
    for (std::size_t k = 0; k < 3; ++k) {
      largest_value = (std::max)(
          {largest_value, std::abs(setup.base_printing_density[k] - base[k]),
           std::abs(setup.grey_printing_density[k] - pivot[k]),
           std::abs(setup.channel_scale[k] - scale[k]), std::abs(setup.grey_code[k] - grey[k]),
           std::abs(bundle_->ScanConstants().lad_aim - setup.grey_code[k] - lad_offset[k])});
    }
    EXPECT_LE(largest_value, 1e-9);
  }
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
  const auto pivot   = setup.F64("adx_grey_printing_density", {n_count, l_count, 3});
  const auto scale   = setup.F64("adx_channel_scale", {n_count, l_count, 3});
  const auto grey    = setup.F64("adx_grey", {n_count, l_count, 3});
  const auto gain    = setup.F64("layer_gain", {n_count, l_count, 3});
  double     largest = 0.0;
  int        checked = 0;
  for (std::size_t n = 0; n < negatives.size(); ++n) {
    for (std::size_t l = 0; l < lights.size(); ++l) {
      SCOPED_TRACE(negatives[n] + " under " + lights[l]);
      const auto values = bundle_->ScanSelection(negatives[n], lights[l]);
      const auto at     = n * lights.size() + l;
      for (std::size_t k = 0; k < 3; ++k) {
        largest =
            (std::max)({largest, std::abs(values.grey_printing_density[k] - pivot[at * 3 + k]),
                        std::abs(values.channel_scale[k] - scale[at * 3 + k]),
                        std::abs(values.grey_code[k] - grey[at * 3 + k]),
                        std::abs(values.layer_gain[k] - gain[at * 3 + k])});
      }
      // The kernel values of this selection have the same values, in float32.
      FilmKernelParams params{};
      film_print_kernel_tables::FillParams(*bundle_, negatives[n], kFilmLabScanId, lights[l], 65,
                                           params);
      EXPECT_EQ(params.scan, 1U);
      EXPECT_EQ(params.scan_grey_density[1], static_cast<float>(values.grey_printing_density[1]));
      EXPECT_EQ(params.scan_grey_code[2], static_cast<float>(values.grey_code[2]));
      EXPECT_EQ(
          params.scan_code_slope[0],
          static_cast<float>(bundle_->ScanConstants().code_per_density * values.channel_scale[0]));
      ++checked;
    }
  }
  // The constants of the transform are those of aces-dev v1.3.1.
  const auto& constants = bundle_->ScanConstants();
  EXPECT_EQ(constants.printing_paper, "kodak_2383");
  EXPECT_EQ(constants.code_per_density, 8000.0);
  EXPECT_EQ(constants.code_base, 1520.0);
  EXPECT_EQ(constants.lad_aim, 7120.0);
  EXPECT_EQ(constants.code_max, 65535.0);
  EXPECT_EQ(constants.cdd_to_cid[0], 0.75573);
  EXPECT_EQ(constants.exp_to_aces[0], 0.72286);
  EXPECT_EQ(constants.lut_cid.size(), 11U);
  std::cout << "scan setup of " << checked << " selections: largest absolute difference "
            << largest << "\n";
  EXPECT_EQ(checked, 30 * static_cast<int>(lights.size()));
  EXPECT_LE(largest, 1e-9);
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
    const auto setup    = bundle_->ScanSelection(blend.negative, mix);
    const auto spectrum = file.F64("spectrum", {bundle_->Wavelengths()});
    double     largest  = 0.0;
    for (std::size_t w = 0; w < spectrum.size(); ++w) {
      largest = (std::max)(largest, std::abs(setup.scene_spectrum[w] - spectrum[w]));
    }
    const auto pivot  = file.F64("adx_grey_printing_density", {3});
    const auto scale  = file.F64("adx_channel_scale", {3});
    const auto grey   = file.F64("adx_grey", {3});
    const auto gain   = file.F64("layer_gain", {3});
    const auto offset = file.F64("lad_offset", {3});
    for (std::size_t k = 0; k < 3; ++k) {
      largest = (std::max)(
          {largest, std::abs(setup.grey_printing_density[k] - pivot[k]),
           std::abs(setup.channel_scale[k] - scale[k]), std::abs(setup.grey_code[k] - grey[k]),
           std::abs(setup.layer_gain[k] - gain[k]),
           std::abs(bundle_->ScanConstants().lad_aim - setup.grey_code[k] - offset[k])});
    }
    std::cout << "As Shot " << blend.negative << " at " << blend.kelvin
              << " K: largest absolute difference " << largest << "\n";
    EXPECT_LE(largest, 1e-9);
  }
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
  // The document list has Lab Scan first, and then the print materials of the bundle.
  ASSERT_EQ(bundle_->PrintMaterials().size() + 1, kFilmPrintMaterialIds.size());
  EXPECT_EQ(kFilmPrintMaterialIds[0], kFilmLabScanId);
  EXPECT_EQ(bundle_->LabScan().id, kFilmLabScanId);
  for (const auto& material : bundle_->PrintMaterials()) {
    EXPECT_TRUE(IsFilmIdentifier(kFilmPrintMaterialIds, material.id)) << material.id;
    EXPECT_NE(material.id, kFilmLabScanId);
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
  EXPECT_EQ(bundle_->LabScan().display_name, kFilmPrintMaterialNames[0]);
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
  (void)tables.Resolve("kodak_5207", kFilmLabScanId, "reference");
  const auto scan = tables.Stats();
  std::cout << "scan u'v' table: " << scan.last_uv_seconds
            << " s; printing density table: " << scan.last_scan_table_seconds << " s\n";
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
