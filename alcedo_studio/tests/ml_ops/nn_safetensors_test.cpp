//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Backend-neutral safetensors parser tests (no CUDA / OpenCL link required).

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <json.hpp>

#include "nn/safetensors.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;

auto FindModelPath(const char* filename) -> fs::path {
  const char* candidates[] = {
#ifdef ALCEDO_DEMOASICNET_MODEL_DIR
      ALCEDO_DEMOASICNET_MODEL_DIR "/",
#endif
      "alcedo_studio/src/config/models/",
      "../alcedo_studio/src/config/models/",
      "../../alcedo_studio/src/config/models/",
      "../../../alcedo_studio/src/config/models/",
      "src/config/models/",
      "../src/config/models/",
  };
  for (const char* prefix : candidates) {
    fs::path path = fs::path(prefix) / filename;
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
      return path;
    }
  }
  return {};
}

void WriteSafetensorsFile(const fs::path& path, const nlohmann::json& header,
                          const std::vector<char>& data) {
  const std::string header_str = header.dump();
  const std::uint64_t header_len = static_cast<std::uint64_t>(header_str.size());

  std::ofstream out(path, std::ios::binary);
  ASSERT_TRUE(out) << "failed to open temp file for write: " << path;
  out.write(reinterpret_cast<const char*>(&header_len), sizeof(header_len));
  out.write(header_str.data(), static_cast<std::streamsize>(header_str.size()));
  if (!data.empty()) {
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
  }
  out.close();
  ASSERT_TRUE(out) << "failed writing safetensors fixture: " << path;
}

void ExpectKeyShape(const nn::SafetensorsTensorMap& map, const char* key,
                    std::initializer_list<std::int64_t> shape) {
  ASSERT_TRUE(map.contains(key)) << "missing key: " << key;
  const auto& t = map.at(key);
  EXPECT_EQ(t.dtype, nn::SafetensorsTensor::Dtype::F32) << key;
  EXPECT_TRUE(t.ShapeEquals(shape)) << key;
  EXPECT_EQ(t.data.size(), t.numel()) << key;
  EXPECT_NO_THROW({
    const auto& ref = nn::RequireF32Tensor(map, key, shape);
    EXPECT_EQ(&ref, &t);
  }) << key;
}

void ExpectBayerShapeTable(const nn::SafetensorsTensorMap& map) {
  ExpectKeyShape(map, "pack.weight", {4, 3, 2, 2});
  ExpectKeyShape(map, "trunk.0.weight", {24, 4, 3, 3});
  ExpectKeyShape(map, "trunk.0.bias", {24});
  for (int i = 1; i < 8; ++i) {
    const std::string w = "trunk." + std::to_string(i) + ".weight";
    const std::string b = "trunk." + std::to_string(i) + ".bias";
    ExpectKeyShape(map, w.c_str(), {24, 24, 3, 3});
    ExpectKeyShape(map, b.c_str(), {24});
  }
  ExpectKeyShape(map, "residual.weight", {12, 24, 1, 1});
  ExpectKeyShape(map, "residual.bias", {12});
  ExpectKeyShape(map, "unpack.weight", {12, 1, 2, 2});
  ExpectKeyShape(map, "post_conv.weight", {24, 6, 3, 3});
  ExpectKeyShape(map, "post_conv.bias", {24});
  ExpectKeyShape(map, "output.weight", {3, 24, 1, 1});
  ExpectKeyShape(map, "output.bias", {3});
}

void ExpectXtransShapeTable(const nn::SafetensorsTensorMap& map) {
  ExpectKeyShape(map, "pack.weight", {12, 3, 2, 2});
  ExpectKeyShape(map, "trunk.0.weight", {32, 12, 3, 3});
  ExpectKeyShape(map, "trunk.0.bias", {32});
  for (int i = 1; i < 4; ++i) {
    const std::string w = "trunk." + std::to_string(i) + ".weight";
    const std::string b = "trunk." + std::to_string(i) + ".bias";
    ExpectKeyShape(map, w.c_str(), {32, 32, 3, 3});
    ExpectKeyShape(map, b.c_str(), {32});
  }
  ExpectKeyShape(map, "residual.weight", {12, 32, 1, 1});
  ExpectKeyShape(map, "residual.bias", {12});
  ExpectKeyShape(map, "unpack.weight", {12, 1, 2, 2});
  ExpectKeyShape(map, "post_conv.weight", {32, 6, 3, 3});
  ExpectKeyShape(map, "post_conv.bias", {32});
  ExpectKeyShape(map, "output.weight", {3, 32, 1, 1});
  ExpectKeyShape(map, "output.bias", {3});
}

}  // namespace

// Purpose: real Bayer student model metadata and full F32 tensor inventory.
TEST(NnSafetensorsTest, AcceptsRealBayerStudentMetadataAndTensorInventory) {
  const auto path = FindModelPath("bayer.safetensors");
  if (path.empty()) {
    GTEST_SKIP() << "bayer.safetensors not found under config/models";
  }

  nn::SafetensorsTensorMap map;
  ASSERT_NO_THROW(map = nn::LoadSafetensors(path));

  EXPECT_EQ(map.metadata("format"), "demosaicnet-pytorch-state_dict");
  EXPECT_EQ(map.metadata("variant"), "bayer");
  EXPECT_EQ(map.metadata("architecture"), "bayer_s24_d8");
  EXPECT_EQ(map.metadata("tile_pad"), "32");
  EXPECT_EQ(map.metadata("tile_border"), "31");
  EXPECT_EQ(map.metadata("tile_step"), "1024");
  EXPECT_EQ(map.size(), 24u);
  ExpectBayerShapeTable(map);
  EXPECT_TRUE(nn::ShapesEqual(map.at("post_conv.weight").shape, {24, 6, 3, 3}));
}

// Purpose: real X-Trans student model metadata and full F32 tensor inventory.
TEST(NnSafetensorsTest, AcceptsRealXtransStudentMetadataAndTensorInventory) {
  const auto path = FindModelPath("xtrans.safetensors");
  if (path.empty()) {
    GTEST_SKIP() << "xtrans.safetensors not found under config/models";
  }

  nn::SafetensorsTensorMap map;
  ASSERT_NO_THROW(map = nn::LoadSafetensors(path));

  EXPECT_EQ(map.metadata("format"), "demosaicnet-pytorch-state_dict");
  EXPECT_EQ(map.metadata("variant"), "xtrans");
  EXPECT_EQ(map.metadata("architecture"), "xtrans_p2_s32_d4");
  EXPECT_EQ(map.metadata("tile_pad"), "12");
  EXPECT_EQ(map.metadata("tile_step"), "1020");
  EXPECT_EQ(map.size(), 16u);
  ExpectXtransShapeTable(map);
}

// Purpose: RequireF32Tensor rejects missing keys (missing tensor).
TEST(NnSafetensorsTest, RequireF32TensorRejectsMissingTensor) {
  const auto path = FindModelPath("bayer.safetensors");
  if (path.empty()) {
    GTEST_SKIP() << "bayer.safetensors not found";
  }
  const auto map = nn::LoadSafetensors(path);
  EXPECT_THROW(nn::RequireF32Tensor(map, "does.not.exist", {1}), std::runtime_error);
}

// Purpose: RequireF32Tensor rejects rank / dimension mismatches.
TEST(NnSafetensorsTest, RequireF32TensorRejectsRankAndDimensionMismatch) {
  const auto path = FindModelPath("bayer.safetensors");
  if (path.empty()) {
    GTEST_SKIP() << "bayer.safetensors not found";
  }
  const auto map = nn::LoadSafetensors(path);
  EXPECT_THROW(nn::RequireF32Tensor(map, "pack.weight", {4, 3, 3, 3}), std::runtime_error);
  EXPECT_THROW(nn::RequireF32Tensor(map, "pack.weight", {4, 3, 2}), std::runtime_error);
  EXPECT_NO_THROW(nn::RequireF32Tensor(map, "pack.weight", {4, 3, 2, 2}));
}

class NnSafetensorsFixtureTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / "alcedo_nn_safetensors_test";
    std::error_code ec;
    fs::create_directories(dir_, ec);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  auto TempPath(std::string_view name) const -> fs::path { return dir_ / std::string(name); }

  fs::path dir_;
};

// Purpose: missing file is a hard parse error.
TEST_F(NnSafetensorsFixtureTest, RejectsMissingFile) {
  EXPECT_THROW(nn::LoadSafetensors(TempPath("does_not_exist.safetensors")), std::runtime_error);
}

// Purpose: non-F32 dtype is rejected before payload interpretation.
TEST_F(NnSafetensorsFixtureTest, RejectsNonF32Dtype) {
  const auto path = TempPath("f16.safetensors");
  nlohmann::json header = {
      {"w", {{"dtype", "F16"}, {"shape", {2}}, {"data_offsets", {0, 4}}}},
  };
  std::vector<char> data(4, 0);
  WriteSafetensorsFile(path, header, data);
  try {
    nn::LoadSafetensors(path);
    FAIL() << "expected non-F32 rejection";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("F32"), std::string::npos) << e.what();
  }
}

// Purpose: data_offsets byte range must match F32 shape product.
TEST_F(NnSafetensorsFixtureTest, RejectsByteRangeMismatchVsShape) {
  const auto path = TempPath("bad_nbytes.safetensors");
  nlohmann::json header = {
      {"w", {{"dtype", "F32"}, {"shape", {3}}, {"data_offsets", {0, 8}}}},
  };
  std::vector<char> data(8, 0);
  WriteSafetensorsFile(path, header, data);
  EXPECT_THROW(nn::LoadSafetensors(path), std::runtime_error);
}

// Purpose: truncated tensor data section is rejected.
TEST_F(NnSafetensorsFixtureTest, RejectsTruncatedTensorData) {
  const auto path = TempPath("trunc_data.safetensors");
  nlohmann::json header = {
      {"w", {{"dtype", "F32"}, {"shape", {4}}, {"data_offsets", {0, 16}}}},
  };
  std::vector<char> data(8, 0);
  WriteSafetensorsFile(path, header, data);
  EXPECT_THROW(nn::LoadSafetensors(path), std::runtime_error);
}

// Purpose: negative shape dimensions are rejected.
TEST_F(NnSafetensorsFixtureTest, RejectsNegativeShapeDimension) {
  const auto path = TempPath("neg_shape.safetensors");
  nlohmann::json header = {
      {"w", {{"dtype", "F32"}, {"shape", {-1}}, {"data_offsets", {0, 0}}}},
  };
  WriteSafetensorsFile(path, header, {});
  EXPECT_THROW(nn::LoadSafetensors(path), std::runtime_error);
}

// Purpose: inverted data_offsets (end < start) are rejected.
TEST_F(NnSafetensorsFixtureTest, RejectsInvertedDataOffsets) {
  const auto path = TempPath("inverted_off.safetensors");
  nlohmann::json header = {
      {"w", {{"dtype", "F32"}, {"shape", {1}}, {"data_offsets", {8, 0}}}},
  };
  WriteSafetensorsFile(path, header, {});
  EXPECT_THROW(nn::LoadSafetensors(path), std::runtime_error);
}

// Purpose: duplicate tensor names in the header are rejected (no silent overwrite).
TEST_F(NnSafetensorsFixtureTest, RejectsDuplicateTensorName) {
  // JSON object keys are unique in nlohmann::json parse, so exercise Insert directly.
  nn::SafetensorsTensorMap map;
  nn::SafetensorsTensor a;
  a.name  = "dup";
  a.dtype = nn::SafetensorsTensor::Dtype::F32;
  a.shape = {1};
  a.data  = {1.0f};
  map.Insert(std::move(a));

  nn::SafetensorsTensor b;
  b.name  = "dup";
  b.dtype = nn::SafetensorsTensor::Dtype::F32;
  b.shape = {1};
  b.data  = {2.0f};
  EXPECT_THROW(map.Insert(std::move(b)), std::runtime_error);
  EXPECT_FLOAT_EQ(map.at("dup").data[0], 1.0f);
}

// Purpose: synthetic F32 payload round-trips host values exactly.
TEST_F(NnSafetensorsFixtureTest, LoadsSyntheticF32RoundTripValues) {
  const auto path = TempPath("tiny_ok.safetensors");
  const std::vector<float> values = {1.0f, -2.5f, 3.25f, 0.0f};
  std::vector<char> data(values.size() * sizeof(float));
  std::memcpy(data.data(), values.data(), data.size());

  nlohmann::json header = {
      {"__metadata__", {{"format", "test"}, {"variant", "unit"}}},
      {"tiny.weight",
       {{"dtype", "F32"},
        {"shape", {2, 2}},
        {"data_offsets", {0, static_cast<int>(data.size())}}}},
  };
  WriteSafetensorsFile(path, header, data);

  const auto map = nn::LoadSafetensors(path);
  EXPECT_EQ(map.size(), 1u);
  EXPECT_EQ(map.metadata("format"), "test");
  const auto& t = nn::RequireF32Tensor(map, "tiny.weight", {2, 2});
  ASSERT_EQ(t.data.size(), values.size());
  for (std::size_t i = 0; i < values.size(); ++i) {
    EXPECT_FLOAT_EQ(t.data[i], values[i]);
  }
}

}  // namespace alcedo
