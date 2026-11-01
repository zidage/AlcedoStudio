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
void WriteSyntheticResources(const fs::path& directory) {
  fs::create_directories(directory);
  std::map<std::string, ArrayValues> stock;
  stock["common/wavelengths_nm"] = F64Array({kWavelengths}, Flat(500.0));
  stock["common/print_contrast"] = F64Array({}, {0.85});
  stock["common/print_flash"]    = F64Array({}, {0.02});
  stock["common/viewing_flare"]  = F64Array({}, {0.003});
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
  }
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
    stock[name("curve_density")]     = F64Array(
        {3, 5}, {0.1, 0.5, 1.0, 1.6, 2.1, 0.1, 0.6, 1.1, 1.7, 2.1, 0.1, 0.4, 0.9, 1.5, 1.5});
    stock[name("curve_lengths")]     = ArrayValues{"i8", {3}, ToBytes<std::int64_t>({5, 5, 4})};
    stock[name("curve_log_h_start")] = F64Array({3}, {-3.0, -3.0, -3.0});
    stock[name("curve_log_h_step")]  = F64Array({}, {1.5});
  }
  const nlohmann::json metadata{
      {"negatives", {{"neg_a", {{"name", "Negative A"}}}, {"neg_b", {{"name", "Negative B"}}}}},
      {"print_materials", {{"paper_a", {{"name", "Paper A"}}}, {"paper_b", {{"name", "Paper B"}}}}},
      {"scene_lights", {"reference", "daylight"}},
      {"scene_spectra", {"flat", "ramp"}}};
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
  setup["xyz_to_display"]       = F64Array({2, 3, 3}, std::vector<double>(18, 0.01));
  setup["scene_spectrum_index"] = ArrayValues{"i4", {2, 2}, ToBytes<std::int32_t>({0, 1, 1, 0})};
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

struct LabSelection {
  fs::path    path;
  std::string negative;
  std::string print_material;
  std::string scene_light;
};

auto LabSelections() -> std::vector<LabSelection> {
  std::vector<LabSelection> selections;
  for (const auto& entry : fs::directory_iterator(fs::path(ALCEDO_FILM_PRINT_EXPECTED_DIR))) {
    const auto file     = FilmArrayFile::Parse(ReadBytes(entry.path()), entry.path());
    const auto metadata = file.Metadata();
    selections.push_back({entry.path(), metadata.at("negative").get<std::string>(),
                          metadata.at("print_material").get<std::string>(),
                          metadata.at("scene_light").get<std::string>()});
  }
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
  const auto selections = LabSelections();
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

TEST_F(FilmPrintTablesLabTest, UvTableMatchesLab) {
  const auto selections = LabSelections();
  ASSERT_GE(selections.size(), 3u);
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
  const auto selections = LabSelections();
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
  EXPECT_EQ(bundle_->PrintMaterials().size(), 18u);
  EXPECT_EQ(bundle_->SceneLights().size(), 11u);
  EXPECT_EQ(bundle_->SceneLights()[0], "reference");
  EXPECT_NO_THROW((void)bundle_->Selection("kodak_5207", "kodak_2383", "reference"));
}

TEST_F(FilmPrintTablesLabTest, ShippedListsMatchTheIdentifiersOfTheDocumentModel) {
  ASSERT_EQ(bundle_->Negatives().size(), kFilmNegativeIds.size());
  for (const auto& negative : bundle_->Negatives()) {
    EXPECT_TRUE(IsFilmIdentifier(kFilmNegativeIds, negative.id)) << negative.id;
  }
  ASSERT_EQ(bundle_->PrintMaterials().size(), kFilmPrintMaterialIds.size());
  for (const auto& material : bundle_->PrintMaterials()) {
    EXPECT_TRUE(IsFilmIdentifier(kFilmPrintMaterialIds, material.id)) << material.id;
  }
  ASSERT_EQ(bundle_->SceneLights().size(), kFilmSceneLightIds.size());
  for (const auto& light : bundle_->SceneLights()) {
    EXPECT_TRUE(IsFilmIdentifier(kFilmSceneLightIds, light)) << light;
  }
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
}

#else

TEST(FilmPrintTablesLabTest, LabValuesAreConfigured) {
  GTEST_SKIP() << "ALCEDO_FILM_PRINT_RESOURCE_DIR and ALCEDO_FILM_PRINT_EXPECTED_DIR are not "
                  "configured, so the comparison with the lab did not run.";
}

#endif

}  // namespace
}  // namespace alcedo
