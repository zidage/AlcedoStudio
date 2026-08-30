// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#pragma once
#include "decoded_rgb_test_support.hpp"
#include "edit/runtime/dng_profile_gpu_data.hpp"
#include "edit/runtime/dng_profile_gpu_math.h"

namespace alcedo::gpu_dag_test {
inline void SaveDngRender(const cv::Mat& display, const char* backend, const char* label) {
  if (const char* dir = std::getenv("ALCEDO_DNG_RENDER_OUTPUT")) {
    cv::Mat preview;
    cv::resize(display, preview, cv::Size(), .2, .2, cv::INTER_AREA);
    cv::cvtColor(preview, preview, cv::COLOR_RGBA2RGB);
    preview.convertTo(preview, CV_8U, 255);
    QImage image(preview.data, preview.cols, preview.rows, static_cast<qsizetype>(preview.step),
                 QImage::Format_RGB888);
    ASSERT_TRUE(image.save(QString::fromStdString(
        (std::filesystem::path(dir) / (std::string(backend) + "_" + label + ".png")).string())));
  }
}

/// Real Bayer DNG: full-resolution GPU profile, persisted graph, and color-only cache invalidation.
template <class Device>
void VerifyCanonDngProfile(const char* backend) {
  const auto path =
      std::filesystem::path(TEST_IMG_PATH) / "raw/camera/canon/r6iii/9327411796_dng.dng";
  if (!std::filesystem::exists(path)) GTEST_SKIP() << "Private Canon fixture missing";
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  ASSERT_TRUE(file.good());
  std::vector<std::byte> bytes(static_cast<std::size_t>(file.tellg()));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  auto  input = RawInputLoader::LoadEncoded(bytes, DecodeRes::FULL);
  Image image(1, path, ImageType::DNG);
  MetadataExtractor::ExtractEXIF_ToImage(path, image);
  auto document = CreateDefaultPipelineDocument();
  ApplyImportedCameraProfile(document, image.GetRawColorContext());
  auto payload = document.Develop()->Params().Params();
  ASSERT_TRUE(payload.camera_profile.dng_profile);
  // Fix the demosaic algorithm so this regression isolates DNG color processing.
  payload.demosaic_method        = "legacy";
  payload.highlights_reconstruct = false;
  document.Develop()->Params().ReplaceParams(payload);
  const auto plan = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
  Device     device;
  auto       output = device.Execute(plan, input, document);
  device.WaitIdle();
  auto display = DownloadRgb(device, output);
  ASSERT_EQ(display.cols, input.develop_output_extent.width);
  ASSERT_EQ(display.rows, input.develop_output_extent.height);
  ASSERT_TRUE(cv::checkRange(display));
  SaveDngRender(display, backend, "profile");

  // Sample GPU results against scalar table evaluation, independently of the output transform.
  {
    const cv::Mat sensor    = DownloadRgb(device, plan.geometry_output);
    const cv::Mat developed = DownloadRgb(device, plan.develop_output);
    const auto transform = ResolveDevelopColorTransform(payload);
    ASSERT_TRUE(transform.ok);
    const auto table  = PackDngProfileGpuData(payload.camera_profile, transform.transform);
    const auto encode = [](float x) {
      if (x < 0) return (std::log2(1.0f / 65536) + 9.72f) / 17.52f + x;
      return (std::log2(x < 1.0f / 32768 ? 1.0f / 65536 + x * .5f : x) + 9.72f) / 17.52f;
    };
    for (int y = 41; y < sensor.rows; y += 193)
      for (int x = 31; x < sensor.cols; x += 211) {
        const auto s    = sensor.at<cv::Vec4f>(y, x);
        float      c[3] = {};
        for (int r = 0; r < 3; ++r)
          for (int k = 0; k < 3; ++k) c[r] += transform.transform.camera_to_ap1[r * 3 + k] * s[k];
        const auto corrected = DngApplyColorProfile(DngMakeRgb(c[0], c[1], c[2]), table.data());
        const auto actual    = developed.at<cv::Vec4f>(y, x);
        ASSERT_NEAR(actual[0], encode(corrected.r), 3e-4);
        ASSERT_NEAR(actual[1], encode(corrected.g), 3e-4);
        ASSERT_NEAR(actual[2], encode(corrected.b), 3e-4);
      }
  }
  auto restored = PipelineDocument::FromJson(document.ToJson());
  auto count    = device.PassStats().camera_color_execute;
  output        = device.Execute(plan, input, restored);
  device.WaitIdle();
  EXPECT_EQ(device.PassStats().camera_color_execute, count);
  EXPECT_LT(cv::norm(display, DownloadRgb(device, output), cv::NORM_INF), 1e-6);

  // Remove only the tables in a diagnostic copy, preserving matrix/calibration and exposure.
  auto without_tables                       = *payload.camera_profile.dng_profile;
  without_tables.hue_sat_map_1              = {};
  without_tables.hue_sat_map_2              = {};
  without_tables.look_table                 = {};
  auto matrix_payload                       = payload;
  matrix_payload.camera_profile.dng_profile = MakeDngColorProfile(without_tables);
  document.Develop()->Params().ReplaceParams(matrix_payload);
  const auto sensor_count = device.PassStats().sensor_develop_execute;
  output                  = device.Execute(plan, input, document);
  device.WaitIdle();
  auto matrix_display = DownloadRgb(device, output);
  ASSERT_TRUE(cv::checkRange(matrix_display));
  EXPECT_EQ(device.PassStats().sensor_develop_execute, sensor_count);
  EXPECT_EQ(device.PassStats().camera_color_execute, count + 1);
  EXPECT_GT(cv::norm(display, matrix_display, cv::NORM_L1) / (display.total() * 3), .005);
  SaveDngRender(matrix_display, backend, "matrix_only");
  matrix_display.release();
  display.release();

  // The editor's default highlight reconstruction must also work with the complete profile.
  payload.highlights_reconstruct = true;
  document.Develop()->Params().ReplaceParams(payload);
  output = device.Execute(plan, input, document);
  device.WaitIdle();
  display = DownloadRgb(device, output);
  ASSERT_TRUE(cv::checkRange(display));
  SaveDngRender(display, backend, "profile_hlr");
}
}  // namespace alcedo::gpu_dag_test
