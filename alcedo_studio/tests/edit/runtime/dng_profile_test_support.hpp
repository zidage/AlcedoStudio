// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#pragma once
#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

#include "../../image/dcp_test_file_writer.hpp"
#include "decoded_rgb_test_support.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/runtime/aces_reference_gamut_compression.h"
#include "edit/runtime/dng_profile_gpu_data.hpp"
#include "edit/runtime/dng_profile_gpu_math.h"
#include "edit/runtime/execution_plan.hpp"
#include "image/dcp_file_reader.hpp"
#include "image/dng_color_profile_cache.hpp"
#include "image/metadata_extractor.hpp"

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

/// ACEScc encoding of the camera pass output, for comparisons against the scalar reference.
inline auto EncodeAcesCc(float x) -> float {
  if (x < 0) return (std::log2(1.0f / 65536) + 9.72f) / 17.52f + x;
  return (std::log2(x < 1.0f / 32768 ? 1.0f / 65536 + x * .5f : x) + 9.72f) / 17.52f;
}

/**
 * @brief Compare camera pass samples of @p device with scalar `DngApplyColorProfile` on the
 * effective camera profile of @p payload (the DCP tables when a reference is bound).
 *
 * Reads the sensor result (`geometry_output`) and the camera pass result (`develop_output`) of the
 * last execution of @p plan. The tolerance is the one of VerifyCanonDngProfile.
 */
template <class Device>
void ExpectCameraPassMatchesScalarProfile(Device& device, const ExecutionPlan& plan,
                                          const DevelopPayload& payload, const char* backend) {
  const cv::Mat sensor    = DownloadRgb(device, plan.geometry_output);
  const cv::Mat developed = DownloadRgb(device, plan.develop_output);
  const auto    transform = ResolveDevelopColorTransform(payload);
  ASSERT_TRUE(transform.ok) << backend;
  const auto table = PackDngProfileGpuData(payload, transform.transform);
  ASSERT_GT(table[0], 0.0f) << backend << ": the effective profile packed no table";
  for (int y = 41; y < sensor.rows; y += 193)
    for (int x = 31; x < sensor.cols; x += 211) {
      const auto s    = sensor.at<cv::Vec4f>(y, x);
      float      c[3] = {};
      for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) c[r] += transform.transform.camera_to_ap1[r * 3 + k] * s[k];
      const auto corrected  = DngApplyColorProfile(DngMakeRgb(c[0], c[1], c[2]), table.data());
      // The camera pass applies the ACES reference gamut compression before ACEScc encoding.
      const auto compressed = AcesReferenceGamutCompress(corrected.r, corrected.g, corrected.b);
      const auto actual     = developed.at<cv::Vec4f>(y, x);
      ASSERT_NEAR(actual[0], EncodeAcesCc(compressed.r), 3e-4)
          << backend << " (" << x << ", " << y << ")";
      ASSERT_NEAR(actual[1], EncodeAcesCc(compressed.g), 3e-4)
          << backend << " (" << x << ", " << y << ")";
      ASSERT_NEAR(actual[2], EncodeAcesCc(compressed.b), 3e-4)
          << backend << " (" << x << ", " << y << ")";
    }
}

/**
 * @brief Synthetic DCP file of the DCP plan, phase C7: two illuminants with forward matrices, two
 * hue and saturation maps, a look table with sRGB value encoding, a `BaselineExposureOffset`, and
 * a `ProfileToneCurve`. No vendor profile bytes.
 */
inline auto SyntheticDcpProfileFile() -> test::DcpTestFileWriter {
  constexpr std::uint16_t kForwardMatrix1           = 50964;
  constexpr std::uint16_t kForwardMatrix2           = 50965;
  constexpr std::uint16_t kColorMatrix2             = 50722;
  constexpr std::uint16_t kCalibrationIlluminant1   = 50778;
  constexpr std::uint16_t kCalibrationIlluminant2   = 50779;
  constexpr std::uint16_t kProfileHueSatMapDims     = 50937;
  constexpr std::uint16_t kProfileHueSatMapData1    = 50938;
  constexpr std::uint16_t kProfileHueSatMapData2    = 50939;
  constexpr std::uint16_t kProfileToneCurve         = 50940;
  constexpr std::uint16_t kProfileLookTableDims     = 50981;
  constexpr std::uint16_t kProfileLookTableData     = 50982;
  constexpr std::uint16_t kProfileLookTableEncoding = 51108;
  constexpr std::uint16_t kBaselineExposureOffset   = 51109;
  // Hue, saturation, and value corrections with saturation varying fastest. The zero-saturation
  // division of each hue keeps value scale 1, as DNG requires for the neutral axis.
  const auto              table = [](std::uint32_t hues, std::uint32_t sats, std::uint32_t values,
                        float hue_shift, float sat_step) {
    std::vector<float> entries;
    for (std::uint32_t v = 0; v < values; ++v)
      for (std::uint32_t h = 0; h < hues; ++h)
        for (std::uint32_t s = 0; s < sats; ++s) {
          entries.push_back(hue_shift * static_cast<float>((h % 3) + 1));
          entries.push_back(1.0f + sat_step * static_cast<float>(s));
          entries.push_back(s == 0 ? 1.0f : 0.92f + 0.04f * static_cast<float>(v));
        }
    return entries;
  };
  auto writer = test::MinimalDcpProfile("Alcedo C7 Camera", "Alcedo C7 Standard");
  writer
      .SRationals(kColorMatrix2,
                  {0.6875, -0.097, -0.0932, -0.4691, 1.2459, 0.2501, -0.0874, 0.1953, 0.5809})
      .Shorts(kCalibrationIlluminant1, {17})
      .Shorts(kCalibrationIlluminant2, {21})
      .SRationals(kForwardMatrix1,
                  {0.7978, 0.1352, 0.0313, 0.288, 0.7119, 0.0001, 0.0, 0.0, 0.8251})
      .SRationals(kForwardMatrix2,
                  {0.7978, 0.1352, 0.0313, 0.288, 0.7119, 0.0001, 0.0, -0.0001, 0.8253})
      .Longs(kProfileHueSatMapDims, {6, 4, 1})
      .Floats(kProfileHueSatMapData1, table(6, 4, 1, 3.0f, 0.06f))
      .Floats(kProfileHueSatMapData2, table(6, 4, 1, -4.0f, 0.04f))
      .Longs(kProfileLookTableDims, {9, 5, 3})
      .Floats(kProfileLookTableData, table(9, 5, 3, 2.5f, -0.03f))
      .Longs(kProfileLookTableEncoding, {1})
      .SRationalPairs(kBaselineExposureOffset, {{1, 4}})
      .Floats(kProfileToneCurve, {0.0f, 0.0f, 0.18f, 0.42f, 0.6f, 0.86f, 1.0f, 1.0f});
  return writer;
}

/**
 * @brief Real Bayer RAW file with a synthetic DCP profile (DCP plan, phase C7).
 *
 * Renders with the import profile, selects the DCP profile, and renders again on the same device.
 * The second render reuses the sensor result and runs the camera pass once more; its samples equal
 * scalar `DngApplyColorProfile` on the DCP tables. Clearing the selection renders the pixels of
 * the import profile again.
 */
template <class Device>
void VerifyDcpCameraProfile(const char* backend) {
  const auto path = std::filesystem::path(TEST_IMG_PATH) / "ci_rawfiles" /
                    "Tag @ryanbreitkreutz - Free files from @signatureeditscoDSC00830.ARW";
  if (!std::filesystem::exists(path)) GTEST_SKIP() << "CI RAW fixture missing: " << path;
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  ASSERT_TRUE(file.good());
  std::vector<std::byte> bytes(static_cast<std::size_t>(file.tellg()));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  auto  input = RawInputLoader::LoadEncoded(bytes, DecodeRes::FULL);
  Image image(1, path, ImageType::ARW);
  MetadataExtractor::ExtractEXIF_ToImage(path, image);
  ASSERT_TRUE(image.HasRawColorContext());
  auto document = CreateDefaultPipelineDocument();
  ApplyImportedCameraProfile(document, image.GetRawColorContext());
  auto payload = document.Develop()->Params().Params();
  ASSERT_FALSE(payload.camera_profile.dng_profile) << "an ARW file has no embedded DNG profile";
  // Fix the demosaic algorithm so this regression isolates the camera color pass.
  payload.demosaic_method        = "legacy";
  payload.highlights_reconstruct = false;
  document.Develop()->Params().ReplaceParams(payload);

  const auto plan = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
  Device     device;
  auto       output = device.Execute(plan, input, document);
  device.WaitIdle();
  const cv::Mat import_display = DownloadRgb(device, output);
  ASSERT_EQ(import_display.cols, input.develop_output_extent.width);
  ASSERT_EQ(import_display.rows, input.develop_output_extent.height);
  ASSERT_TRUE(cv::checkRange(import_display));
  SaveDngRender(import_display, backend, "dcp_import_profile");

  const auto dcp_path =
      std::filesystem::temp_directory_path() / (std::string("alcedo_c7_dcp_") + backend + ".dcp");
  SyntheticDcpProfileFile().Write(dcp_path);
  const auto      dcp = ReadDcpCameraProfile(dcp_path);
  std::error_code removed;
  std::filesystem::remove(dcp_path, removed);
  ASSERT_TRUE(dcp && dcp->tables && dcp->profile_tone_curve);
  ASSERT_TRUE(dcp->forward_matrices_valid);

  const auto sensor_count = device.PassStats().sensor_develop_execute;
  const auto camera_count = device.PassStats().camera_color_execute;
  document.Develop()->Params().ApplyCameraProfileFileUpdate({dcp});
  const auto dcp_payload = document.Develop()->Params().Params();
  ASSERT_TRUE(dcp_payload.camera_profile_file.has_value());
  ASSERT_TRUE(dcp_payload.camera_profile_file->profile_tone);
  const auto dcp_plan = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
  output              = device.Execute(dcp_plan, input, document);
  device.WaitIdle();
  EXPECT_EQ(device.PassStats().sensor_develop_execute, sensor_count)
      << backend << ": selecting a DCP profile decoded the sensor again";
  EXPECT_EQ(device.PassStats().camera_color_execute, camera_count + 1);
  const cv::Mat dcp_display = DownloadRgb(device, output);
  ASSERT_EQ(dcp_display.size(), import_display.size());
  ASSERT_TRUE(cv::checkRange(dcp_display));
  EXPECT_GT(cv::norm(import_display, dcp_display, cv::NORM_L1) / (import_display.total() * 3), .005)
      << backend << ": the DCP profile left the display unchanged";
  SaveDngRender(dcp_display, backend, "dcp_profile");
  ExpectCameraPassMatchesScalarProfile(device, dcp_plan, dcp_payload, backend);
  if (::testing::Test::HasFatalFailure()) return;

  // Clearing the reference renders the import profile again, still from the cached sensor result.
  document.Develop()->Params().ApplyCameraProfileFileUpdate({});
  ASSERT_FALSE(document.Develop()->Params().Params().camera_profile_file.has_value());
  output = device.Execute(plan, input, document);
  device.WaitIdle();
  EXPECT_EQ(device.PassStats().sensor_develop_execute, sensor_count);
  EXPECT_LT(cv::norm(import_display, DownloadRgb(device, output), cv::NORM_INF), 1e-6)
      << backend << ": clearing the DCP profile did not restore the import render";
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

  // Project data stores only the profile fingerprint. A document read back from its JSON is
  // unbound; after the profile is loaded from the source file through the profile cache, a
  // fresh device (no shared result cache) must render the same pixels as the import-bound document.
  {
    auto reloaded = PipelineDocument::FromJson(document.ToJson());
    ASSERT_TRUE(reloaded.Develop()->Params().DngProfile().IsReferenced());
    ASSERT_FALSE(reloaded.Develop()->Params().DngProfile().IsBound());
    DngColorProfileCache cache([](const std::filesystem::path& source) {
      return MetadataExtractor::ReadDngColorProfileFromSource(source);
    });
    reloaded.Develop()->Params().BindDngColorProfile(cache.Load(path));
    ASSERT_EQ(reloaded.ToJson(), document.ToJson());
    Device     reload_device;
    const auto reload_output = reload_device.Execute(plan, input, reloaded);
    reload_device.WaitIdle();
    const auto reload_display = DownloadRgb(reload_device, reload_output);
    ASSERT_EQ(reload_display.size(), display.size());
    EXPECT_LT(cv::norm(display, reload_display, cv::NORM_INF), 1e-6)
        << backend << ": source-bound DNG profile changed the rendered pixels";
  }

  // Sample GPU results against scalar table evaluation, independently of the output transform.
  ExpectCameraPassMatchesScalarProfile(device, plan, payload, backend);
  if (::testing::Test::HasFatalFailure()) return;
  auto restored = PipelineDocument::FromJson(document.ToJson());
  ASSERT_EQ(restored.ToJson(), document.ToJson());
  auto count    = device.PassStats().camera_color_execute;
  output        = device.Execute(plan, input, document);
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
