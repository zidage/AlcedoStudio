//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Tests of UtNet2DirectMlSession (AI Denoise integration plan, DN2) against the expected outputs
// of DN0 (alcedo-denoise-lab, release export-utnet2-bayer-dark-loss-10k).
//
// The folder of the environment variable ALCEDO_AI_DENOISE_TEST_ASSETS holds:
//   utnet2_bayer_dark_loss_10k_fp16.onnx   the FP16 graph (export_fp16_onnx.py)
//   utnet2_bayer_dark_loss_10k.onnx        the FP32 graph (export_onnx.py)
//   random_64.safetensors, random_256.safetensors, rawnind_crop_512.safetensors
// Each expected output file holds `input` (4, h, w) and `output` (3, 2h, 2w) in float32: the
// PyTorch IEEE FP32 output of the model.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "decoders/processor/nn/utnet2_directml_session.hpp"
#include "decoders/processor/nn/utnet2_specs.hpp"
#include "nn/safetensors.hpp"

namespace alcedo::nn {
namespace {

constexpr const char* kAssetsVariable = "ALCEDO_AI_DENOISE_TEST_ASSETS";
constexpr const char* kFp16GraphName  = "utnet2_bayer_dark_loss_10k_fp16.onnx";
constexpr const char* kFp32GraphName  = "utnet2_bayer_dark_loss_10k.onnx";
// max |session - expected| <= kToleranceRelative x max |expected|. The FP16 graph on DirectML
// gives 2.8e-3 to 4.1e-3 (fp16_calibration.json of the lab, 2026-11-22); the tolerance is 1.5
// times the largest, rounded up to one significant digit.
constexpr double      kToleranceRelative = 7e-3;

auto AssetsFolder() -> std::optional<std::filesystem::path> {
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

class UtNet2DirectMlSessionTest : public ::testing::Test {
 protected:
  static void TearDownTestSuite() { session_.reset(); }

  void SetUp() override {
    const auto folder = AssetsFolder();
    if (!folder) {
      GTEST_SKIP() << "Set " << kAssetsVariable
                   << " to the folder with the UtNet2 graphs and the DN0 expected outputs.";
    }
    folder_ = *folder;
    if (!session_) {
      session_ = std::make_unique<UtNet2DirectMlSession>(folder_ / kFp16GraphName);
    }
  }

  auto Run(const ExpectedOutput& expected) -> std::vector<float> {
    std::vector<float> output(OutputCount(expected.tile));
    session_->Run(expected.input, expected.tile, output);
    return output;
  }

  std::filesystem::path                         folder_;
  static inline std::unique_ptr<UtNet2DirectMlSession> session_;
};

TEST_F(UtNet2DirectMlSessionTest, OutputMatchesPyTorchForEachExpectedOutput) {
  for (const auto* name : {"random_64", "random_256", "rawnind_crop_512"}) {
    const auto expected   = LoadExpected(folder_, name);
    const auto output     = Run(expected);
    const auto difference = RelativeDifference(output, expected.output);
    std::cout << name << ": max |difference| = " << difference << " of the largest value\n";
    EXPECT_LE(difference, kToleranceRelative) << name;
  }
}

TEST_F(UtNet2DirectMlSessionTest, LoadFailsOnDifferentArchitectureMetadata) {
  // The value has the same length, so the protobuf of the copy stays valid.
  auto       bytes = ReadBytes(folder_ / kFp16GraphName);
  const auto value = std::string(UtNet2Spec::kArchitecture);
  const auto at    = bytes.find(value);
  ASSERT_NE(at, std::string::npos);
  ASSERT_EQ(bytes.find(value, at + 1), std::string::npos) << "the value occurs more than once";
  bytes.replace(at, value.size(), "utnet2_bayer_funit33");
  const auto copy = TempFolder("architecture") / "different_architecture.onnx";
  WriteBytes(copy, bytes);

  const auto message = MessageOf([&] { UtNet2DirectMlSession session(copy); });
  EXPECT_NE(message.find("`architecture` is `utnet2_bayer_funit33`"), std::string::npos)
      << message;
  EXPECT_NE(message.find(copy.filename().string()), std::string::npos) << message;
}

TEST_F(UtNet2DirectMlSessionTest, LoadFailsOnFloat32Graph) {
  const auto message =
      MessageOf([&] { UtNet2DirectMlSession session(folder_ / kFp32GraphName); });
  EXPECT_NE(message.find("is not float16"), std::string::npos) << message;
  EXPECT_NE(message.find(kFp32GraphName), std::string::npos) << message;
}

TEST_F(UtNet2DirectMlSessionTest, LoadFailsOnMissingFile) {
  const auto absent  = TempFolder("missing") / "absent.onnx";
  const auto message = MessageOf([&] { UtNet2DirectMlSession session(absent); });
  EXPECT_NE(message.find("does not exist"), std::string::npos) << message;
  EXPECT_NE(message.find(absent.filename().string()), std::string::npos) << message;
}

TEST_F(UtNet2DirectMlSessionTest, RunRejectsSizeThatIsNotAMultipleOfSixteen) {
  const PackedTileRect tile{.x = 0, .y = 0, .width = 64, .height = 72};
  std::vector<float>   input(std::size_t{UtNet2Spec::kInputChannels} * 64 * 72, 0.1f);
  std::vector<float>   output(OutputCount(tile), 7.0f);

  const auto message = MessageOf([&] { session_->Run(input, tile, output); });
  EXPECT_NE(message.find("multiples of 16"), std::string::npos) << message;
  EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float v) { return v == 7.0f; }));
}

TEST_F(UtNet2DirectMlSessionTest, RunThrowsOnAnOutputThatIsNotFinite) {
  // 6e4 is a finite float16 value; the activations of the first layers overflow FP16.
  const PackedTileRect tile{.x = 128, .y = 32, .width = 64, .height = 64};
  std::vector<float>   input(std::size_t{UtNet2Spec::kInputChannels} * 64 * 64, 6e4f);
  std::vector<float>   output(OutputCount(tile));

  const auto message = MessageOf([&] { session_->Run(input, tile, output); });
  EXPECT_NE(message.find("not finite"), std::string::npos) << message;
  EXPECT_NE(message.find("UtNet2 tile at (128, 32) of 64 x 64 packed px"), std::string::npos)
      << message;
}

TEST_F(UtNet2DirectMlSessionTest, ReleaseFreesTheSessionAndALaterRunLoadsAgain) {
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

}  // namespace
}  // namespace alcedo::nn
