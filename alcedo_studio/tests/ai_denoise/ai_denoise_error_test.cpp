//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Tests of the typed AI Denoise render error, the platform check, and the sensor layout of a RAW
// file (integration plan, DN7).

#include "edit/input/ai_denoise_error.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

#include "ai_denoise_test_support.hpp"
#include "edit/input/ai_denoise_model_session.hpp"
#include "edit/input/raw_sensor_layout.hpp"

namespace alcedo {
namespace {

using ai_denoise_test::TestPlatform;
using ai_denoise_test::TestPlatformRecord;

TEST(AiDenoiseErrorTest, RenderMessageKeepsTheKindAndTheTextOfTheError) {
  for (const auto kind : {AiDenoiseErrorKind::ModelNotInstalled, AiDenoiseErrorKind::DeviceTooSlow,
                          AiDenoiseErrorKind::RunFailed, AiDenoiseErrorKind::SensorNotSupported}) {
    const AiDenoiseError error(kind, "AI Denoise: the text: (with) parentheses");
    EXPECT_EQ(error.Kind(), kind);
    EXPECT_EQ(error.Text(), "AI Denoise: the text: (with) parentheses");
    EXPECT_EQ(std::string(error.what()), "AI Denoise error (" +
                                             std::string(AiDenoiseErrorKindToken(kind)) +
                                             "): AI Denoise: the text: (with) parentheses");

    // The render message as the scheduler passes it on, and with text that a caller put before.
    const auto parsed = ParseAiDenoiseRenderError(error.what());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, kind);
    EXPECT_EQ(parsed->text, error.Text());
    const auto prefixed = ParseAiDenoiseRenderError(std::string("Render failed: ") + error.what());
    ASSERT_TRUE(prefixed.has_value());
    EXPECT_EQ(*prefixed, *parsed);
  }
}

TEST(AiDenoiseErrorTest, MessageWithoutAnAiDenoiseErrorHasNone) {
  EXPECT_FALSE(ParseAiDenoiseRenderError("").has_value());
  EXPECT_FALSE(ParseAiDenoiseRenderError("Pipeline returned an empty result").has_value());
  EXPECT_FALSE(ParseAiDenoiseRenderError("AI Denoise: the plain text of DN6").has_value());
  EXPECT_FALSE(ParseAiDenoiseRenderError("AI Denoise error (unknown_kind): text").has_value());
  EXPECT_FALSE(ParseAiDenoiseRenderError("AI Denoise error (run_failed").has_value());
}

TEST(AiDenoiseErrorTest, PlatformIsAvailableWhenItsDeviceAndRuntimeLoad) {
  auto                record = std::make_shared<TestPlatformRecord>();
  DenoiseModelSession available(std::make_unique<TestPlatform>(record));
  EXPECT_TRUE(available.IsPlatformAvailable());
  EXPECT_TRUE(available.IsPlatformAvailable());
  // The check runs once in the process.
  EXPECT_EQ(record->runtime_checks.load(), 1);

  auto failing                = std::make_shared<TestPlatformRecord>();
  failing->runtime_load_error = "onnxruntime.dll failed to load";
  DenoiseModelSession without_runtime(std::make_unique<TestPlatform>(failing));
  EXPECT_FALSE(without_runtime.IsPlatformAvailable());
  EXPECT_FALSE(without_runtime.IsPlatformAvailable());
  EXPECT_EQ(failing->runtime_checks.load(), 1);

  DenoiseModelSession no_platform(nullptr);
  EXPECT_FALSE(no_platform.IsPlatformAvailable());
}

// The sample RAW files are local files of the developer (sample_images is not in the
// repository); a test without its file skips.
TEST(AiDenoiseErrorTest, SensorLayoutOfABayerRawFileIsBayer) {
  if (!std::filesystem::exists(ALCEDO_AI_DENOISE_TEST_RAW)) {
    GTEST_SKIP() << "Sample RAW file absent: " << ALCEDO_AI_DENOISE_TEST_RAW;
  }
  EXPECT_EQ(ReadRawSensorLayout(ALCEDO_AI_DENOISE_TEST_RAW), RawSensorLayout::Bayer);
}

TEST(AiDenoiseErrorTest, SensorLayoutOfALinearDngIsNotBayer) {
  if (!std::filesystem::exists(ALCEDO_AI_DENOISE_TEST_LINEAR_DNG)) {
    GTEST_SKIP() << "Sample DNG file absent: " << ALCEDO_AI_DENOISE_TEST_LINEAR_DNG;
  }
  // Demosaiced RGB data (LibRaw filters 0): the loader develops it as decoded RGB.
  EXPECT_EQ(ReadRawSensorLayout(ALCEDO_AI_DENOISE_TEST_LINEAR_DNG), RawSensorLayout::Other);
}

TEST(AiDenoiseErrorTest, SensorLayoutOfAFileThatLibRawCannotOpenIsAnError) {
  const auto missing =
      std::filesystem::path(ALCEDO_AI_DENOISE_TEST_TEMP_ROOT) / "no_such_photo.ARW";
  EXPECT_THROW((void)ReadRawSensorLayout(missing), std::runtime_error);
}

}  // namespace
}  // namespace alcedo
