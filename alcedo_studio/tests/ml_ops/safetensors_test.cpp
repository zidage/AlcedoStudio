//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cuda_runtime.h>

#include <json.hpp>

#include "cuda/nn/device_buffer.hpp"
#include "cuda/nn/safetensors.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

// Resolve bundled model path relative to common build / source layouts.
auto FindModelPath(const char* filename) -> fs::path {
  const char* candidates[] = {
      "alcedo_studio/src/config/models/",
      "../alcedo_studio/src/config/models/",
      "../../alcedo_studio/src/config/models/",
      "../../../alcedo_studio/src/config/models/",
      "src/config/models/",
      "../src/config/models/",
      "D:/Projects/pu-erh_lab/alcedo_studio/src/config/models/",
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

// Write a minimal little-endian safetensors file for negative / unit tests.
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

// Authoritative student shapes: bayer_s24_d8 / xtrans_p2_s32_d4 (Phase 8A).
void ExpectKeyShape(const cuda::nn::SafetensorsTensorMap& map, const char* key,
                    std::initializer_list<std::int64_t> shape) {
  ASSERT_TRUE(map.contains(key)) << "missing key: " << key;
  const auto& t = map.at(key);
  EXPECT_EQ(t.dtype, cuda::nn::SafetensorsTensor::Dtype::F32) << key;
  EXPECT_TRUE(t.ShapeEquals(shape)) << key;
  EXPECT_EQ(t.data.size(), t.numel()) << key;
  EXPECT_NO_THROW({
    const auto& ref = cuda::nn::RequireF32Tensor(map, key, shape);
    EXPECT_EQ(&ref, &t);
  }) << key;
}

void ExpectBayerShapeTable(const cuda::nn::SafetensorsTensorMap& map) {
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

void ExpectXtransShapeTable(const cuda::nn::SafetensorsTensorMap& map) {
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

// ---------------------------------------------------------------------------
// Real bundled models (primary acceptance for Phase 4)
// ---------------------------------------------------------------------------

TEST(MlOpsSafetensorsTest, LoadRealBayerWeightsShapesAndMetadata) {
  const auto path = FindModelPath("bayer.safetensors");
  if (path.empty()) {
    GTEST_SKIP() << "bayer.safetensors not found under config/models";
  }

  cuda::nn::SafetensorsTensorMap map;
  ASSERT_NO_THROW(map = cuda::nn::LoadSafetensors(path));

  EXPECT_EQ(map.metadata("format"), "demosaicnet-pytorch-state_dict");
  EXPECT_EQ(map.metadata("variant"), "bayer");
  EXPECT_EQ(map.metadata("architecture"), "bayer_s24_d8");
  EXPECT_EQ(map.metadata("tile_pad"), "32");
  EXPECT_EQ(map.metadata("tile_border"), "31");
  EXPECT_EQ(map.metadata("tile_step"), "1024");

  // Student Bayer: 12 weight + 12 bias = 24 tensors (pack/unpack bias-free).
  EXPECT_EQ(map.size(), 24u);
  ExpectBayerShapeTable(map);

  // Iteration covers every stored tensor name.
  std::size_t seen = 0;
  for (const auto& [name, tensor] : map) {
    EXPECT_EQ(name, tensor.name);
    EXPECT_FALSE(name.empty());
    ++seen;
  }
  EXPECT_EQ(seen, map.size());

  EXPECT_TRUE(map.contains("pack.weight"));
  EXPECT_TRUE(map.contains("unpack.weight"));
  EXPECT_TRUE(map.contains("residual.weight"));
  EXPECT_FALSE(map.contains("pack.bias"));
  EXPECT_FALSE(map.contains("unpack.bias"));
  EXPECT_TRUE(cuda::nn::ShapesEqual(map.at("post_conv.weight").shape, {24, 6, 3, 3}));
}

TEST(MlOpsSafetensorsTest, LoadRealXtransWeightsShapesAndMetadata) {
  const auto path = FindModelPath("xtrans.safetensors");
  if (path.empty()) {
    GTEST_SKIP() << "xtrans.safetensors not found under config/models";
  }

  cuda::nn::SafetensorsTensorMap map;
  ASSERT_NO_THROW(map = cuda::nn::LoadSafetensors(path));

  EXPECT_EQ(map.metadata("format"), "demosaicnet-pytorch-state_dict");
  EXPECT_EQ(map.metadata("variant"), "xtrans");
  EXPECT_EQ(map.metadata("architecture"), "xtrans_p2_s32_d4");
  EXPECT_EQ(map.metadata("tile_pad"), "12");
  EXPECT_EQ(map.metadata("tile_step"), "1020");

  // Student X-Trans: 8 weight + 8 bias = 16 tensors.
  EXPECT_EQ(map.size(), 16u);
  ExpectXtransShapeTable(map);

  EXPECT_TRUE(map.contains("pack.weight"));
  EXPECT_TRUE(map.contains("unpack.weight"));
  EXPECT_TRUE(map.contains("residual.weight"));
  EXPECT_TRUE(cuda::nn::ShapesEqual(map.at("post_conv.weight").shape, {32, 6, 3, 3}));
  EXPECT_TRUE(cuda::nn::ShapesEqual(map.at("trunk.0.weight").shape, {32, 12, 3, 3}));
}

TEST(MlOpsSafetensorsTest, RealBayerTensorPayloadIsNonTrivial) {
  // Spot-check that we actually copied payload bytes, not empty shells.
  const auto path = FindModelPath("bayer.safetensors");
  if (path.empty()) {
    GTEST_SKIP() << "bayer.safetensors not found";
  }
  const auto map = cuda::nn::LoadSafetensors(path);
  const auto& w  = map.at("pack.weight");
  ASSERT_EQ(w.data.size(), 4u * 3u * 2u * 2u);

  // Fixed one-hot pack has non-zeros; learned trunk weights are non-trivial too.
  bool any_nonzero = false;
  for (float v : w.data) {
    if (v != 0.0f) {
      any_nonzero = true;
      break;
    }
  }
  EXPECT_TRUE(any_nonzero);

  EXPECT_EQ(map.at("trunk.0.bias").data.size(), 24u);
  EXPECT_EQ(map.at("output.bias").data.size(), 3u);
  EXPECT_EQ(map.at("residual.bias").data.size(), 12u);
}

// ---------------------------------------------------------------------------
// Lookup helpers
// ---------------------------------------------------------------------------

TEST(MlOpsSafetensorsTest, RequireF32TensorFailsOnMissingKey) {
  const auto path = FindModelPath("bayer.safetensors");
  if (path.empty()) {
    GTEST_SKIP() << "bayer.safetensors not found";
  }
  const auto map = cuda::nn::LoadSafetensors(path);
  EXPECT_THROW(cuda::nn::RequireF32Tensor(map, "does.not.exist", {1}), std::runtime_error);
}

TEST(MlOpsSafetensorsTest, RequireF32TensorFailsOnWrongShape) {
  const auto path = FindModelPath("bayer.safetensors");
  if (path.empty()) {
    GTEST_SKIP() << "bayer.safetensors not found";
  }
  const auto map = cuda::nn::LoadSafetensors(path);
  // Real shape is [4, 3, 2, 2].
  EXPECT_THROW(cuda::nn::RequireF32Tensor(map, "pack.weight", {4, 3, 3, 3}), std::runtime_error);
  EXPECT_THROW(cuda::nn::RequireF32Tensor(map, "pack.weight", {4, 3, 2}), std::runtime_error);
  EXPECT_NO_THROW(cuda::nn::RequireF32Tensor(map, "pack.weight", {4, 3, 2, 2}));
}

TEST(MlOpsSafetensorsTest, FindAndAtBehavior) {
  const auto path = FindModelPath("xtrans.safetensors");
  if (path.empty()) {
    GTEST_SKIP() << "xtrans.safetensors not found";
  }
  const auto map = cuda::nn::LoadSafetensors(path);
  EXPECT_NE(map.find("output.weight"), nullptr);
  EXPECT_EQ(map.find("nope"), nullptr);
  EXPECT_THROW(map.at("nope"), std::runtime_error);
  EXPECT_FALSE(map.contains("nope"));
  EXPECT_TRUE(map.contains("output.bias"));
}

// ---------------------------------------------------------------------------
// H2D upload helper (bit-identical round-trip)
// ---------------------------------------------------------------------------

class MlOpsSafetensorsCudaTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
  }
};

TEST_F(MlOpsSafetensorsCudaTest, UploadToDeviceRoundTripRealTensor) {
  const auto path = FindModelPath("bayer.safetensors");
  if (path.empty()) {
    GTEST_SKIP() << "bayer.safetensors not found";
  }
  const auto  map = cuda::nn::LoadSafetensors(path);
  const auto& t   = cuda::nn::RequireF32Tensor(map, "residual.weight", {12, 24, 1, 1});

  auto device = cuda::nn::UploadToDevice(t);
  ASSERT_EQ(device.size(), t.data.size());
  const auto host = device.Download();
  ASSERT_EQ(host.size(), t.data.size());
  // Bit-identical for pure H2D/D2H of F32 weights.
  EXPECT_EQ(std::memcmp(host.data(), t.data.data(), host.size() * sizeof(float)), 0);
}

TEST_F(MlOpsSafetensorsCudaTest, UploadToExistingBufferAsync) {
  const auto path = FindModelPath("xtrans.safetensors");
  if (path.empty()) {
    GTEST_SKIP() << "xtrans.safetensors not found";
  }
  const auto  map = cuda::nn::LoadSafetensors(path);
  const auto& t   = cuda::nn::RequireF32Tensor(map, "output.bias", {3});

  cudaStream_t stream = nullptr;
  ASSERT_EQ(::cudaStreamCreate(&stream), cudaSuccess);

  cuda::nn::DeviceBufferF32 buf(t.data.size());
  ASSERT_NO_THROW(cuda::nn::UploadTo(buf, t, stream));
  std::vector<float> host(t.data.size(), -1.0f);
  buf.Download(host.data(), host.size(), stream);
  ASSERT_EQ(::cudaStreamSynchronize(stream), cudaSuccess);
  ::cudaStreamDestroy(stream);

  EXPECT_EQ(std::memcmp(host.data(), t.data.data(), host.size() * sizeof(float)), 0);
}

TEST_F(MlOpsSafetensorsCudaTest, UploadToRejectsSizeMismatch) {
  const auto path = FindModelPath("bayer.safetensors");
  if (path.empty()) {
    GTEST_SKIP() << "bayer.safetensors not found";
  }
  const auto  map = cuda::nn::LoadSafetensors(path);
  const auto& t   = map.at("output.bias");
  cuda::nn::DeviceBufferF32 wrong(t.data.size() + 1);
  EXPECT_THROW(cuda::nn::UploadTo(wrong, t), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Synthetic negative cases (truncated / bad header / non-F32)
// ---------------------------------------------------------------------------

class MlOpsSafetensorsFixtureTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / "alcedo_safetensors_test";
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

TEST_F(MlOpsSafetensorsFixtureTest, RejectsMissingFile) {
  EXPECT_THROW(cuda::nn::LoadSafetensors(TempPath("does_not_exist.safetensors")),
               std::runtime_error);
}

TEST_F(MlOpsSafetensorsFixtureTest, RejectsTruncatedHeaderLenOnly) {
  const auto path = TempPath("trunc_header_len.safetensors");
  {
    std::ofstream out(path, std::ios::binary);
    // Only 4 of 8 bytes.
    const char partial[] = {1, 0, 0, 0};
    out.write(partial, 4);
  }
  EXPECT_THROW(cuda::nn::LoadSafetensors(path), std::runtime_error);
}

TEST_F(MlOpsSafetensorsFixtureTest, RejectsTruncatedJsonHeader) {
  const auto path = TempPath("trunc_json.safetensors");
  {
    std::ofstream out(path, std::ios::binary);
    const std::uint64_t header_len = 64;
    out.write(reinterpret_cast<const char*>(&header_len), sizeof(header_len));
    // Write fewer bytes than header_len claims.
    const char junk[] = "{\"a\":";
    out.write(junk, sizeof(junk) - 1);
  }
  EXPECT_THROW(cuda::nn::LoadSafetensors(path), std::runtime_error);
}

TEST_F(MlOpsSafetensorsFixtureTest, RejectsInvalidJsonHeader) {
  const auto path = TempPath("bad_json.safetensors");
  {
    std::ofstream out(path, std::ios::binary);
    const std::string header = "{not valid json";
    const std::uint64_t header_len = static_cast<std::uint64_t>(header.size());
    out.write(reinterpret_cast<const char*>(&header_len), sizeof(header_len));
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
  }
  EXPECT_THROW(cuda::nn::LoadSafetensors(path), std::runtime_error);
}

TEST_F(MlOpsSafetensorsFixtureTest, RejectsNonF32Dtype) {
  const auto path = TempPath("f16.safetensors");
  // 2 F16 values = 4 bytes; claim F16 so parser must reject before interpreting payload.
  nlohmann::json header = {
      {"w", {{"dtype", "F16"}, {"shape", {2}}, {"data_offsets", {0, 4}}}},
  };
  std::vector<char> data(4, 0);
  WriteSafetensorsFile(path, header, data);
  try {
    cuda::nn::LoadSafetensors(path);
    FAIL() << "expected non-F32 rejection";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("F32"), std::string::npos)
        << "error should mention F32 requirement: " << e.what();
  }
}

TEST_F(MlOpsSafetensorsFixtureTest, RejectsTruncatedTensorData) {
  const auto path = TempPath("trunc_data.safetensors");
  // Shape product wants 4 floats = 16 bytes; only provide 8.
  nlohmann::json header = {
      {"w", {{"dtype", "F32"}, {"shape", {4}}, {"data_offsets", {0, 16}}}},
  };
  std::vector<char> data(8, 0);
  WriteSafetensorsFile(path, header, data);
  EXPECT_THROW(cuda::nn::LoadSafetensors(path), std::runtime_error);
}

TEST_F(MlOpsSafetensorsFixtureTest, RejectsByteSizeMismatchVsShape) {
  const auto path = TempPath("bad_nbytes.safetensors");
  // shape [3] wants 12 bytes, but offsets claim 8.
  nlohmann::json header = {
      {"w", {{"dtype", "F32"}, {"shape", {3}}, {"data_offsets", {0, 8}}}},
  };
  std::vector<char> data(8, 0);
  WriteSafetensorsFile(path, header, data);
  EXPECT_THROW(cuda::nn::LoadSafetensors(path), std::runtime_error);
}

TEST_F(MlOpsSafetensorsFixtureTest, LoadsSyntheticF32RoundTripValues) {
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

  const auto map = cuda::nn::LoadSafetensors(path);
  EXPECT_EQ(map.size(), 1u);
  EXPECT_EQ(map.metadata("format"), "test");
  EXPECT_EQ(map.metadata("variant"), "unit");

  const auto& t = cuda::nn::RequireF32Tensor(map, "tiny.weight", {2, 2});
  ASSERT_EQ(t.data.size(), values.size());
  for (std::size_t i = 0; i < values.size(); ++i) {
    EXPECT_FLOAT_EQ(t.data[i], values[i]);
  }
}

TEST_F(MlOpsSafetensorsFixtureTest, ZeroLengthTensorAllowed) {
  const auto path = TempPath("empty_tensor.safetensors");
  nlohmann::json header = {
      {"empty", {{"dtype", "F32"}, {"shape", {0}}, {"data_offsets", {0, 0}}}},
  };
  WriteSafetensorsFile(path, header, {});
  const auto map = cuda::nn::LoadSafetensors(path);
  const auto& t  = cuda::nn::RequireF32Tensor(map, "empty", {0});
  EXPECT_TRUE(t.data.empty());
  EXPECT_EQ(t.numel(), 0u);
}

}  // namespace alcedo
