//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Tests of the platform session of the FP16 UtNet2 graph (AI Denoise integration plan, DN2, DN3,
// DN10) against the expected outputs of DN0 (alcedo-denoise-lab, release
// export-utnet2-bayer-dark-loss-10k): UtNet2DirectMlSession on Windows (UtNet2DirectMlTest),
// UtNet2CoreMlSession on macOS (UtNet2CoreMlTest).
//
// The folder of the environment variable ALCEDO_AI_DENOISE_TEST_ASSETS holds:
//   utnet2_bayer_dark_loss_10k_fp16.onnx   the FP16 graph (export_fp16_onnx.py)
//   utnet2_bayer_dark_loss_10k.onnx        the FP32 graph (export_onnx.py)
//   random_64.safetensors, random_256.safetensors, rawnind_crop_512.safetensors
// Each expected output file holds `input` (4, h, w) and `output` (3, 2h, 2w) in float32: the
// PyTorch IEEE FP32 output of the model.

#ifdef __APPLE__
#include "decoders/processor/nn/utnet2_coreml_session.hpp"
#else
#include "decoders/processor/nn/utnet2_directml_session.hpp"
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "decoders/processor/nn/utnet2_specs.hpp"
#include "decoders/processor/nn/utnet2_tile_loop.hpp"
#include "nn/safetensors.hpp"

namespace alcedo::nn {
namespace {

#ifdef __APPLE__
using UtNet2PlatformSession = UtNet2CoreMlSession;
#else
using UtNet2PlatformSession = UtNet2DirectMlSession;
#endif

constexpr const char* kAssetsVariable    = "ALCEDO_AI_DENOISE_TEST_ASSETS";
constexpr const char* kFp16GraphName     = "utnet2_bayer_dark_loss_10k_fp16.onnx";
constexpr const char* kFp32GraphName     = "utnet2_bayer_dark_loss_10k.onnx";
// max |session - expected| <= kToleranceRelative x max |expected|. The FP16 graph on DirectML
// gives 2.8e-3 to 4.1e-3 (fp16_calibration.json of the lab, 2026-11-22); the tolerance is 1.5
// times the largest, rounded up to one significant digit. The same tolerance on the Neural Engine
// (DN10).
constexpr double      kToleranceRelative = 7e-3;

auto                  AssetsFolder() -> std::optional<std::filesystem::path> {
  const char* value = std::getenv(kAssetsVariable);
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::filesystem::path(value);
}

auto TempFolder(const std::string& name) -> std::filesystem::path {
  const auto folder = std::filesystem::path(ALCEDO_TEST_TEMP_ROOT) / name;
  std::filesystem::remove_all(folder);
  std::filesystem::create_directories(folder);
  return folder;
}

auto ReadBytes(const std::filesystem::path& path) -> std::string {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void WriteBytes(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream stream(path, std::ios::binary);
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

auto RelativeDifference(const std::vector<float>& result, const std::vector<float>& expected)
    -> double {
  double largest_difference = 0.0;
  double largest_expected   = 0.0;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    largest_difference = std::max(largest_difference, std::abs(double{result[i]} - expected[i]));
    largest_expected   = std::max(largest_expected, std::abs(double{expected[i]}));
  }
  return largest_difference / largest_expected;
}

auto MessageOf(const std::function<void()>& action) -> std::string {
  try {
    action();
  } catch (const std::runtime_error& error) {
    return error.what();
  }
  return {};
}

struct ExpectedOutput {
  std::vector<float> input;
  std::vector<float> output;
  PackedTileRect     tile;
};

auto LoadExpected(const std::filesystem::path& folder, const std::string& name) -> ExpectedOutput {
  const auto  tensors = LoadSafetensors(folder / (name + ".safetensors"));
  const auto& input   = tensors.at("input");
  const auto& output  = tensors.at("output");
  if (input.shape.size() != 3 || output.shape.size() != 3) {
    throw std::runtime_error(name + ": expected (4, h, w) and (3, 2h, 2w) tensors");
  }
  ExpectedOutput expected;
  expected.input       = input.data;
  expected.output      = output.data;
  expected.tile.height = static_cast<int>(input.shape[1]);
  expected.tile.width  = static_cast<int>(input.shape[2]);
  return expected;
}

auto OutputCount(const PackedTileRect& tile) -> std::size_t {
  return std::size_t{UtNet2Spec::kOutputChannels} * UtNet2Spec::kOutputUpscale *
         UtNet2Spec::kOutputUpscale * tile.width * tile.height;
}

class UtNet2SessionTest : public ::testing::Test {
 protected:
  static void TearDownTestSuite() { session_.reset(); }

  void        SetUp() override {
    const auto folder = AssetsFolder();
    if (!folder) {
      GTEST_SKIP() << "Set " << kAssetsVariable
                   << " to the folder with the UtNet2 graphs and the DN0 expected outputs.";
    }
    folder_ = *folder;
    if (!session_) {
      session_ = std::make_unique<UtNet2PlatformSession>(folder_ / kFp16GraphName);
    }
  }

  auto Run(const ExpectedOutput& expected) -> std::vector<float> {
    std::vector<float> output(OutputCount(expected.tile));
    session_->Run(expected.input, expected.tile, output);
    return output;
  }

  std::filesystem::path                                folder_;
  static inline std::unique_ptr<UtNet2PlatformSession> session_;
};

TEST_F(UtNet2SessionTest, OutputMatchesPyTorchForEachExpectedOutput) {
  for (const auto* name : {"random_64", "random_256", "rawnind_crop_512"}) {
    const auto expected   = LoadExpected(folder_, name);
    const auto output     = Run(expected);
    const auto difference = RelativeDifference(output, expected.output);
    std::cout << name << ": max |difference| = " << difference << " of the largest value\n";
    EXPECT_LE(difference, kToleranceRelative) << name;
  }
}

TEST_F(UtNet2SessionTest, LoadFailsOnDifferentArchitectureMetadata) {
  // The value has the same length, so the protobuf of the copy stays valid.
  auto       bytes = ReadBytes(folder_ / kFp16GraphName);
  const auto value = std::string(UtNet2Spec::kArchitecture);
  const auto at    = bytes.find(value);
  ASSERT_NE(at, std::string::npos);
  ASSERT_EQ(bytes.find(value, at + 1), std::string::npos) << "the value occurs more than once";
  bytes.replace(at, value.size(), "utnet2_bayer_funit33");
  const auto copy = TempFolder("architecture") / "different_architecture.onnx";
  WriteBytes(copy, bytes);

  const auto message = MessageOf([&] { UtNet2PlatformSession session(copy); });
  EXPECT_NE(message.find("`architecture` is `utnet2_bayer_funit33`"), std::string::npos) << message;
  EXPECT_NE(message.find(copy.filename().string()), std::string::npos) << message;
}

TEST_F(UtNet2SessionTest, LoadFailsOnFloat32Graph) {
  const auto message = MessageOf([&] { UtNet2PlatformSession session(folder_ / kFp32GraphName); });
  EXPECT_NE(message.find("is not float16"), std::string::npos) << message;
  EXPECT_NE(message.find(kFp32GraphName), std::string::npos) << message;
}

TEST_F(UtNet2SessionTest, LoadFailsOnMissingFile) {
  const auto absent  = TempFolder("missing") / "absent.onnx";
  const auto message = MessageOf([&] { UtNet2PlatformSession session(absent); });
  EXPECT_NE(message.find("does not exist"), std::string::npos) << message;
  EXPECT_NE(message.find(absent.filename().string()), std::string::npos) << message;
}

TEST_F(UtNet2SessionTest, RunRejectsSizeThatIsNotAMultipleOfSixteen) {
  const PackedTileRect tile{.x = 0, .y = 0, .width = 64, .height = 72};
  std::vector<float>   input(std::size_t{UtNet2Spec::kInputChannels} * 64 * 72, 0.1f);
  std::vector<float>   output(OutputCount(tile), 7.0f);

  const auto           message = MessageOf([&] { session_->Run(input, tile, output); });
  EXPECT_NE(message.find("multiples of 16"), std::string::npos) << message;
  EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float v) { return v == 7.0f; }));
}

TEST_F(UtNet2SessionTest, RunThrowsOnAnOutputThatIsNotFinite) {
  // 6e4 is a finite float16 value; the activations of the first layers overflow FP16.
  const PackedTileRect tile{.x = 128, .y = 32, .width = 64, .height = 64};
  std::vector<float>   input(std::size_t{UtNet2Spec::kInputChannels} * 64 * 64, 6e4f);
  std::vector<float>   output(OutputCount(tile));

  const auto           message = MessageOf([&] { session_->Run(input, tile, output); });
  EXPECT_NE(message.find("not finite"), std::string::npos) << message;
  EXPECT_NE(message.find("UtNet2 tile at (128, 32) of 64 x 64 packed px"), std::string::npos)
      << message;
}

// The largest difference, relative to the largest value of `one_pass` (3, 1024, 1024), between
// the one-pass output of the 512 crop and the one-pass output of its top left `size` x `size`
// packed px, in the output px that do not depend on the bottom and the right border of the
// smaller input (its size minus the overlap). In exact arithmetic it is 0; in FP16 on DirectML
// and on the Neural Engine it is the rounding difference of a change of the input shape.
auto ShapeChangeDifference(UtNet2PlatformSession& session, const ExpectedOutput& expected,
                           const std::vector<float>& one_pass, int size) -> double {
  const int          full = expected.tile.width;
  std::vector<float> input(std::size_t{4} * size * size);
  for (int channel = 0; channel < 4; ++channel) {
    for (int y = 0; y < size; ++y) {
      std::copy_n(expected.input.begin() + (std::size_t{1} * channel * full + y) * full, size,
                  input.begin() + (std::size_t{1} * channel * size + y) * size);
    }
  }
  const PackedTileRect tile{.x = 0, .y = 0, .width = size, .height = size};
  std::vector<float>   output(OutputCount(tile));
  session.Run(input, tile, output);
  const int interior           = 2 * (size - UtNet2Spec::kTileOverlap);
  double    largest_difference = 0.0;
  double    largest_value      = 0.0;
  for (int channel = 0; channel < 3; ++channel) {
    for (int v = 0; v < interior; ++v) {
      for (int u = 0; u < interior; ++u) {
        const double small = output[(std::size_t{1} * channel * 2 * size + v) * 2 * size + u];
        const double large = one_pass[(std::size_t{1} * channel * 2 * full + v) * 2 * full + u];
        largest_difference = std::max(largest_difference, std::abs(small - large));
      }
    }
  }
  for (const float value : one_pass) {
    largest_value = std::max(largest_value, double{std::abs(value)});
  }
  return largest_difference / largest_value;
}

// DN3: the tile loop on the platform session gives the one-pass output (overlap 96, the last
// tile shrunk).
// One tile through the loop is the one-pass output bit for bit. With tiles of 256, the tiles
// have other shapes than the one pass, and the device rounds FP16 differently for each shape: the
// tiled output may differ from the one-pass output by the difference that a change of the input
// shape gives without tiles (measured 1.8e-3 to 2.5e-3 of the largest value, DN3 completion
// record), with the margin of the DN2 tolerance.
TEST_F(UtNet2SessionTest, TiledOutputEqualsOnePassOnTheExpectedCrop) {
  const auto             expected = LoadExpected(folder_, "rawnind_crop_512");
  const auto             one_pass = Run(expected);
  const PackedMosaicView mosaic{expected.input, expected.tile.width, expected.tile.height};
  const auto             run_loop = [&](int tile_size, std::vector<float>& output, int& tiles) {
    UtNet2TileLoopOptions options;
    options.tile_size = tile_size;
    options.slow_device_limit_seconds_per_megapixel = std::numeric_limits<double>::infinity();
    return DenoiseBayerMosaic(
        *session_, mosaic, RgbPlanesView{output, 2 * mosaic.width, 2 * mosaic.height},
        [&](const UtNet2TileResult&) { ++tiles; }, [] { return false; },
        [] { return std::chrono::steady_clock::now(); }, options);
  };

  std::vector<float> one_tile(one_pass.size(), -1.0f);
  int                one_tile_count = 0;
  EXPECT_EQ(run_loop(512, one_tile, one_tile_count).tile_count, 1);
  EXPECT_EQ(one_tile, one_pass);

  std::vector<float> tiled(one_pass.size(), -1.0f);
  int                tiles  = 0;
  const auto         record = run_loop(256, tiled, tiles);
  // 512 packed px with tiles of 256 and overlap 96: 5 tiles of each axis.
  EXPECT_EQ(record.tile_count, 25);
  EXPECT_EQ(tiles, 25);

  double shape_level = 0.0;
  for (const int size : {256, 384}) {
    shape_level = std::max(shape_level, ShapeChangeDifference(*session_, expected, one_pass, size));
  }
  const auto difference = RelativeDifference(tiled, one_pass);
  std::cout << "tiled against one pass: max |difference| = " << difference
            << " of the largest value; a change of the input shape: " << shape_level << "\n";
  // 1.5 times the measured level (the rule of the DN2 tolerance); 1e-5 (the FP32 level of DN1)
  // when the shapes give the same rounding.
  EXPECT_LE(difference, std::max(1e-5, 1.5 * shape_level));
  EXPECT_LE(RelativeDifference(tiled, expected.output), kToleranceRelative);
}

// DN3, DN10: DirectML compiles the graph at the first run of each input shape, Core ML creates a
// session for each shape, so the slow-device check measures a tile only when its shape ran on the
// loaded session.
TEST_F(UtNet2SessionTest, SessionIsReadyOnlyForATileShapeThatRan) {
  session_->Release();
  EXPECT_FALSE(session_->IsReadyFor(64, 64));

  const auto expected = LoadExpected(folder_, "random_64");
  (void)Run(expected);
  EXPECT_TRUE(session_->IsReadyFor(64, 64));
  EXPECT_FALSE(session_->IsReadyFor(256, 256));
  EXPECT_FALSE(session_->IsReadyFor(64, 128));

  session_->Release();
  EXPECT_FALSE(session_->IsReadyFor(64, 64));
}

TEST_F(UtNet2SessionTest, ReleaseFreesTheSessionAndALaterRunLoadsAgain) {
  const auto expected = LoadExpected(folder_, "random_64");
  const auto before   = Run(expected);
  ASSERT_TRUE(session_->IsLoaded());

  session_->Release();
  EXPECT_FALSE(session_->IsLoaded());

  const auto after = Run(expected);
  EXPECT_TRUE(session_->IsLoaded());
  EXPECT_EQ(after, before);
  EXPECT_LE(RelativeDifference(after, expected.output), kToleranceRelative);
}

#ifdef __APPLE__
// DN10: the compiled Core ML model of each tile shape is in its own folder of the model cache. A
// session of a shape in the cache gives the output of a session that compiled the model.
TEST_F(UtNet2SessionTest, ModelCacheHoldsEachShapeAndGivesTheOutputOfACompiledSession) {
  const auto         cache    = TempFolder("model_cache");
  const auto         expected = LoadExpected(folder_, "random_256");
  const auto         compiled = Run(expected);

  std::vector<float> filled(compiled.size());
  {
    UtNet2CoreMlSession filling(folder_ / kFp16GraphName, cache);
    filling.Run(expected.input, expected.tile, filled);
  }
  ASSERT_TRUE(std::filesystem::is_directory(cache / "256x256"));
  EXPECT_FALSE(std::filesystem::is_empty(cache / "256x256"));
  EXPECT_EQ(filled, compiled);

  std::vector<float>  read(compiled.size());
  UtNet2CoreMlSession reading(folder_ / kFp16GraphName, cache);
  reading.Run(expected.input, expected.tile, read);
  EXPECT_EQ(read, compiled);

  // Another shape gets its own folder, so it does not load the model of 256 x 256.
  const auto         small = LoadExpected(folder_, "random_64");
  std::vector<float> small_output(OutputCount(small.tile));
  reading.Run(small.input, small.tile, small_output);
  EXPECT_TRUE(std::filesystem::is_directory(cache / "64x64"));
  EXPECT_LE(RelativeDifference(small_output, small.output), kToleranceRelative);
}

TEST(UtNet2CoreMlModelCacheTest, PreparedFolderIsTheOnlyEntryOfTheRoot) {
  const auto root = std::filesystem::path(ALCEDO_TEST_TEMP_ROOT) / "model_cache_root";
  std::filesystem::remove_all(root);
  const std::string old_version(64, 'a');
  const std::string version(64, 'b');
  std::filesystem::create_directories(root / old_version / "1536x1536");
  std::filesystem::create_directories(root / "other");

  const auto folder = PrepareCoreMlModelCache(root, version);
  EXPECT_EQ(folder, root / version);
  EXPECT_TRUE(std::filesystem::is_directory(folder));
  EXPECT_FALSE(std::filesystem::exists(root / old_version));
  EXPECT_FALSE(std::filesystem::exists(root / "other"));

  // A version that is not a SHA-256 is never a folder name (it could leave the root).
  for (const auto* invalid : {"..", "../other", "ABCDEF"}) {
    EXPECT_THROW((void)PrepareCoreMlModelCache(root, invalid), std::runtime_error) << invalid;
  }
  EXPECT_TRUE(std::filesystem::is_directory(folder));
}
#endif

}  // namespace
}  // namespace alcedo::nn
