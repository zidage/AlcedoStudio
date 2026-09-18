//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <gtest/gtest.h>

#include <QImage>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <opencv2/imgproc.hpp>
#include <span>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "decoders/processor/raw_processor.hpp"
#include "decoders/processor/raw_rgb_normalization.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "image/image.hpp"
#include "image/metadata_extractor.hpp"

namespace alcedo::gpu_dag_test {

/// The older RAW entry point must perform the same GPU level/WB conversion as the DAG.
inline void VerifyLegacyRgbGpu(RawGpuBackend backend) {
  auto    raw  = std::make_unique<LibRaw>();
  auto&   data = raw->imgdata.rawdata;
  cv::Mat codes(32, 48, CV_16UC4, cv::Scalar(9280, 9280, 9280, 0));
  data.color4_image    = reinterpret_cast<ushort(*)[4]>(codes.data);
  data.sizes.raw_width = data.sizes.width = codes.cols;
  data.sizes.raw_height = data.sizes.height = codes.rows;
  data.sizes.raw_pitch                      = static_cast<unsigned>(codes.step);
  data.color.black                          = 1024;
  data.color.maximum                        = 17536;
  data.color.cam_mul[0]                     = 2.0f;
  data.color.cam_mul[1]                     = 1.0f;
  data.color.cam_mul[2]                     = 1.5f;
  data.color.cam_mul[3]                     = 1.0f;
  data.color.as_shot_wb_applied             = LIBRAW_ASWB_APPLIED | LIBRAW_ASWB_SONY;
  raw->imgdata.color                        = data.color;
  raw->imgdata.idata.colors                 = 3;
  raw->imgdata.idata.filters                = 0;
  RawParams params;
  params.gpu_backend_            = backend;
  params.highlights_reconstruct_ = false;
  const ushort crop[4]           = {};
  RawProcessor processor(params, data, *raw, RawRuntimeColorContext{}, crop);
  auto         output = processor.Process();
  output.SyncToCPU();
  const auto& rgb = output.GetCPUData();
  ASSERT_EQ(rgb.size(), codes.size());
  ASSERT_EQ(rgb.type(), CV_32FC4);
  for (int y = 0; y < rgb.rows; ++y) {
    for (int x = 0; x < rgb.cols; ++x) {
      const auto pixel = rgb.at<cv::Vec4f>(y, x);
      EXPECT_NEAR(pixel[0], 0.25f, 1e-6f);
      EXPECT_NEAR(pixel[1], 0.5f, 1e-6f);
      EXPECT_NEAR(pixel[2], 1.0f / 3.0f, 1e-6f);
      EXPECT_FLOAT_EQ(pixel[3], 1.0f);
    }
  }
}

template <typename Device>
auto DownloadRgb(Device& device, const GraphValueId& id) -> cv::Mat {
  auto* lease = device.Workspace().Images().Find(id);
  if (lease == nullptr) throw std::runtime_error("RGB render did not produce the requested image");
  const auto& texture = lease->Texture();
  cv::Mat     pixels(texture.Height(), texture.Width(), CV_32FC4);
  device.Workspace().Device().DownloadTexture2D(
      texture,
      std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data),
                           pixels.total() * pixels.elemSize()),
      device.CommandContext());
  return pixels;
}

/// Equivalent balanced/unbalanced codes must produce identical complete renders, with HLR on/off.
template <typename Device>
void VerifyRgbWhiteBalanceAndLevels() {
  for (bool highlights : {false, true}) {
    for (bool integer_codes : {false, true}) {
      cv::Mat reference;
      for (int applied :
           {0, LIBRAW_ASWB_APPLIED | LIBRAW_ASWB_SONY, LIBRAW_ASWB_APPLIED | LIBRAW_ASWB_CANON}) {
        SCOPED_TRACE(::testing::Message() << highlights << ',' << integer_codes << ',' << applied);
        auto  raw                = std::make_unique<LibRaw>();
        auto& color              = raw->imgdata.color;
        color.black              = 100;
        color.cblack[1]          = 200;
        color.maximum            = 2100;
        color.cam_mul[0]         = 2.0f;
        color.cam_mul[1]         = 1.0f;
        color.cam_mul[2]         = 1.5f;
        color.as_shot_wb_applied = applied;
        auto        plane        = MakeF32RgbaPlane(48, 32);
        auto*       pixels = const_cast<float*>(reinterpret_cast<const float*>(plane.bytes.get()));
        // Includes black, below-black and highlight headroom samples.
        const float native[] = {-0.1f, 0.0f, 0.2f, 0.5f, 1.2f};
        for (unsigned i = 0; i < 48 * 32; ++i) {
          for (int c = 0; c < 3; ++c) {
            float sample = native[i % 5];
            if (applied) sample *= color.cam_mul[c];
            pixels[i * 4 + c] = integer_codes
                                    ? color.black + color.cblack[c] +
                                          sample * (color.maximum - color.black - color.cblack[c])
                                    : sample;
          }
        }
        auto input              = RawInputLoader::FromDirectRgb(plane, FullSensor(48, 32));
        input.rgb_linearization = raw_norm::BuildRgbLinearization(color, integer_codes);
        std::copy_n(color.cam_mul, 3, input.linearization.cam_mul);
        auto document = CreateDefaultPipelineDocument();
        EnsureTestCameraProfile(document);
        auto payload                   = document.Develop()->Params().Params();
        payload.highlights_reconstruct = highlights;
        // RGB must not try to load or execute a demosaic model.
        payload.demosaic_method        = "neural_engine";
        document.Develop()->Params().ReplaceParams(payload);
        const auto plan = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
        Device     device;
        const auto output = device.Execute(plan, input, document);
        device.WaitIdle();
        const auto display = DownloadRgb(device, output);
        ASSERT_TRUE(cv::checkRange(display));
        if (reference.empty())
          reference = display;
        else
          EXPECT_LT(cv::norm(display, reference, cv::NORM_INF), 3e-5);
        if (!highlights) {
          const cv::Mat sensor = DownloadRgb(device, plan.sensor_linear_output);
          for (int x = 0; x < 5; ++x) {
            const float expected = integer_codes ? std::max(0.0f, native[x]) : native[x];
            for (int c = 0; c < 3; ++c) EXPECT_NEAR(sensor.at<cv::Vec4f>(0, x)[c], expected, 2e-6f);
          }
        }
      }
    }
  }
}

/// DNG opcodes must run for RGB and leave a publishable final sensor image.
template <typename Device>
void VerifyRgbWarpPublishes() {
  auto input    = RawInputLoader::FromDirectRgb(MakeF32RgbaPlane(96, 64), FullSensor(96, 64));
  auto document = CreateDefaultPipelineDocument();
  EnsureTestCameraProfile(document);
  cv::Mat unwarped;
  for (bool warp_enabled : {false, true}) {
    if (warp_enabled) {
      dng::WarpRectilinear warp;
      warp.coefficient_set_count = 1;
      warp.coefficient_sets[0]   = {0.8, 0.0, 0.0, 0.0, 0.0, 0.0};
      input.dng_warp_rectilinear = warp;
    }
    const auto plan = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
    Device     device;
    const auto output = device.Execute(plan, input, document);
    device.WaitIdle();
    const cv::Mat sensor = DownloadRgb(device, plan.sensor_linear_output);
    ASSERT_TRUE(cv::checkRange(DownloadRgb(device, output)));
    if (!warp_enabled)
      unwarped = sensor;
    else {
      EXPECT_GT(cv::norm(sensor, unwarped, cv::NORM_INF), 0.05);
      // Radial scale 0.8 samples closer to the center on both sides.
      EXPECT_GT(sensor.at<cv::Vec4f>(32, 12)[0], unwarped.at<cv::Vec4f>(32, 12)[0]);
      EXPECT_LT(sensor.at<cv::Vec4f>(32, 84)[0], unwarped.at<cv::Vec4f>(32, 84)[0]);
    }
    EXPECT_NO_THROW((void)device.Execute(plan, input, document));
    EXPECT_GT(device.PassStats().sensor_develop_skip, 0U);
  }
}

/// Exercise the reported files through metadata import, decode, all DAG stages and cache reuse.
template <typename Device>
void VerifyCameraRgbFile(const char* filename, ImageType type, const char* backend) {
  const auto path =
      std::filesystem::path(TEST_IMG_PATH) / "raw/camera/sony/a7cii/ycbcr_compressed" / filename;
  if (!std::filesystem::exists(path)) GTEST_SKIP() << "Private camera fixture missing: " << path;
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  ASSERT_TRUE(file.good());
  std::vector<std::byte> bytes(static_cast<std::size_t>(file.tellg()));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  auto input = RawInputLoader::LoadEncoded(bytes, DecodeRes::FULL);
  ASSERT_TRUE(input.rgb_linearization.has_value());
  if (type == ImageType::DNG) ASSERT_TRUE(input.dng_warp_rectilinear.has_value());
  Image image(1, path, type);
  MetadataExtractor::ExtractEXIF_ToImage(path, image);
  ASSERT_TRUE(image.HasRawColorContext());
  auto document = CreateDefaultPipelineDocument();
  ApplyImportedCameraProfile(document, image.GetRawColorContext());
  auto payload                   = document.Develop()->Params().Params();
  payload.highlights_reconstruct = false;
  document.Develop()->Params().ReplaceParams(payload);
  const auto plan = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
  ASSERT_FALSE(plan.Contains(GpuPassKind::Demosaic));
  Device device;
  auto   output = device.Execute(plan, input, document);
  device.WaitIdle();
  auto display = DownloadRgb(device, output);
  ASSERT_EQ(display.cols, input.develop_output_extent.width);
  ASSERT_EQ(display.rows, input.develop_output_extent.height);
  ASSERT_TRUE(cv::checkRange(display));
  EXPECT_GT(cv::norm(display, cv::NORM_L1), 100.0);
  if (type == ImageType::ARW) {
    const cv::Mat sensor = DownloadRgb(device, plan.sensor_linear_output);
    const auto* codes  = reinterpret_cast<const float*>(input.pixels.bytes.get());
    for (int y = 0; y < sensor.rows; y += 37) {
      for (int x = 0; x < sensor.cols; x += 41) {
        for (int c = 0; c < 3; ++c) {
          const float expected = std::max(0.0f, codes[(y * sensor.cols + x) * 4 + c] - 1024.0f) /
                                 16512.0f * input.linearization.cam_mul[1] /
                                 input.linearization.cam_mul[c];
          ASSERT_NEAR(sensor.at<cv::Vec4f>(y, x)[c], expected, 2e-6f);
        }
      }
    }
  }
  EXPECT_NO_THROW((void)device.Execute(plan, input, document));
  EXPECT_GT(device.PassStats().sensor_develop_skip, 0U);
  // Optional local evidence; never write private images into tracked fixtures.
  // Run the editor's default HLR-on setting at full sensor resolution as well.
  payload.highlights_reconstruct = true;
  document.Develop()->Params().ReplaceParams(payload);
  output = device.Execute(plan, input, document);
  device.WaitIdle();
  display = DownloadRgb(device, output);
  ASSERT_TRUE(cv::checkRange(display));
  EXPECT_GT(cv::norm(display, cv::NORM_L1), 100.0);
  if (const char* dir = std::getenv("ALCEDO_RGB_RENDER_OUTPUT")) {
    cv::Mat bgr, preview;
    cv::cvtColor(display, bgr, cv::COLOR_RGBA2BGR);
    cv::resize(bgr, preview, cv::Size(), 0.25, 0.25, cv::INTER_AREA);
    preview.convertTo(preview, CV_8U, 255.0);
    const QImage preview_image(preview.data, preview.cols, preview.rows,
                               static_cast<qsizetype>(preview.step), QImage::Format_BGR888);
    ASSERT_TRUE(preview_image.save(QString::fromStdString(
        (std::filesystem::path(dir) / (std::string(backend) + "_" + filename + ".png")).string())));
  }
}

}  // namespace alcedo::gpu_dag_test
