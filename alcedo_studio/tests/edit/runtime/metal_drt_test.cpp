//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/cst/odt_op.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/metal/metal_drt_pass.hpp"
#include "edit/runtime/metal/metal_pass_encoder.hpp"
#include "metal/compute_pipeline_cache.hpp"

namespace alcedo {
namespace {

struct Rgba {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

auto HasMetalDevice() -> bool {
  try {
    return BindSystemDefaultMetalPresentationDevice() != nullptr;
  } catch (...) {
    return false;
  }
}

auto Download(MetalRenderDevice& device, const GraphValueId& id) -> std::vector<Rgba> {
  auto* lease = device.Workspace().Images().Find(id);
  EXPECT_NE(lease, nullptr);
  if (lease == nullptr) {
    return {};
  }
  const auto&       tex = lease->Texture();
  std::vector<Rgba> pixels(static_cast<std::size_t>(tex.Width()) * tex.Height());
  device.Workspace().Device().DownloadTexture2D(
      tex,
      std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                           pixels.size() * sizeof(Rgba)),
      device.CommandContext());
  return pixels;
}

auto AcesccDecode(float encoded) -> float {
  constexpr float kLog2Min      = -15.0f;
  constexpr float kLog2Denorm   = -16.0f;
  constexpr float kDenormOffset = 0.00001525878906f;
  constexpr float kA            = 9.72f;
  constexpr float kB            = 17.52f;
  const float     encode_floor  = (kLog2Denorm + kA) / kB;
  const float     denorm_thr    = (kLog2Min + kA) / kB;
  if (encoded < encode_floor) {
    return encoded - encode_floor;
  }
  if (encoded <= denorm_thr) {
    return (std::exp2(encoded * kB - kA) - kDenormOffset) * 2.0f;
  }
  return std::exp2(encoded * kB - kA);
}

auto CpuDisplayEncoding(const ColorUtils::TO_OUTPUT_Params& params, float x, float y, float z)
    -> Rgba {
  const auto& m = params.limit_to_display_matx_;
  float       lin[3] = {x * m(0, 0) + y * m(1, 0) + z * m(2, 0),
                        x * m(0, 1) + y * m(1, 1) + z * m(2, 1),
                        x * m(0, 2) + y * m(1, 2) + z * m(2, 2)};
  for (float& c : lin) {
    c = std::max(0.0f, c * params.display_linear_scale_);
  }
  auto encode = [&](float v) {
    switch (params.eotf_) {
      case ColorUtils::EOTF::LINEAR:
        return v;
      case ColorUtils::EOTF::GAMMA_2_2:
        return std::pow(v, 1.0f / 2.2f);
      case ColorUtils::EOTF::GAMMA_2_6:
        return std::pow(v, 1.0f / 2.6f);
      case ColorUtils::EOTF::GAMMA_1_8:
        return std::pow(v, 1.0f / 1.8f);
      default:
        return v;
    }
  };
  return {encode(lin[0]), encode(lin[1]), encode(lin[2]), 1.0f};
}

auto CpuOpenDrt(const ColorUtils::OpenDRTParams& p, float r, float g, float b) -> cv::Vec3f {
  const float ap1_to_xyz[9] = {0.6524187177f, 0.1271799255f, 0.1708572838f,
                               0.2680640592f, 0.6724644790f, 0.0594714618f,
                               -0.0054699285f, 0.0051828000f, 1.0893448793f};
  const float xyz_to_p3[9]  = {2.4934969119f, -0.9313836179f, -0.4027107845f,
                               -0.8294889696f, 1.7626640603f, 0.0236246858f,
                               0.0358458302f, -0.0761723893f, 0.9568845240f};
  auto apply = [](const float* m, float x, float y, float z) {
    return cv::Vec3f(m[0] * x + m[1] * y + m[2] * z, m[3] * x + m[4] * y + m[5] * z,
                     m[6] * x + m[7] * y + m[8] * z);
  };
  cv::Vec3f rgb = apply(ap1_to_xyz, r, g, b);
  rgb           = apply(xyz_to_p3, rgb[0], rgb[1], rgb[2]);
  const cv::Vec3f rs_w(p.rs_rw_, 1.0f - p.rs_rw_ - p.rs_bw_, p.rs_bw_);
  float           sat_l = rgb.dot(rs_w);
  rgb                   = cv::Vec3f(sat_l, sat_l, sat_l) * p.rs_sa_ + rgb * (1.0f - p.rs_sa_);
  rgb += cv::Vec3f(p.tn_off_, p.tn_off_, p.tn_off_);
  float tsn = static_cast<float>(cv::norm(rgb)) / 1.7320508075688772f;
  if (std::fabs(tsn) >= 1.0e-8f) {
    rgb /= tsn;
  } else {
    rgb = cv::Vec3f(0, 0, 0);
  }
  tsn = std::pow(tsn / (tsn + p.ts_s_), p.ts_p_);
  tsn *= p.ts_m2_;
  tsn = (p.tn_toe_ == 0.0f) ? tsn : (tsn * tsn) / (tsn + p.tn_toe_);
  tsn *= p.ts_dsc_;
  rgb *= tsn;
  if (p.clamp_) {
    rgb[0] = std::clamp(rgb[0], 0.0f, 1.0f);
    rgb[1] = std::clamp(rgb[1], 0.0f, 1.0f);
    rgb[2] = std::clamp(rgb[2], 0.0f, 1.0f);
  }
  return rgb;
}

auto AllFinite(const std::vector<Rgba>& pixels) -> bool {
  if (pixels.empty()) {
    return false;
  }
  for (const auto& pixel : pixels) {
    if (!std::isfinite(pixel.r) || !std::isfinite(pixel.g) || !std::isfinite(pixel.b) ||
        !std::isfinite(pixel.a)) {
      return false;
    }
  }
  return true;
}

auto MakeConstantRgb(std::uint32_t width, std::uint32_t height, float value) -> HostImagePlane {
  auto  plane = gpu_dag_test::MakeF32RgbaPlane(width, height);
  auto* px    = const_cast<float*>(reinterpret_cast<const float*>(plane.bytes.get()));
  for (std::uint32_t i = 0; i < width * height; ++i) {
    px[i * 4 + 0] = value;
    px[i * 4 + 1] = value;
    px[i * 4 + 2] = value;
    px[i * 4 + 3] = 1.0f;
  }
  return plane;
}

class MetalDrtFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasMetalDevice()) {
      GTEST_SKIP() << "No Metal device available.";
    }
    (void)BindSystemDefaultMetalPresentationDevice();
    document_ = CreateDefaultPipelineDocument();
    gpu_dag_test::EnsureTestCameraProfile(document_);
    input_ = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                           gpu_dag_test::FullSensor(16, 12));
  }

  auto Render() -> GraphValueId {
    plan_ = GraphCompiler::Compile(document_, input_.CompileSource(), RenderRequest{});
    return device_.Execute(plan_, input_, document_);
  }

  PipelineDocument   document_;
  PreparedRawInput   input_;
  MetalRenderDevice  device_;
  ExecutionPlan      plan_;
};

TEST_F(MetalDrtFixture, MetalDrtOpenDrtMatchesCudaReferenceWithinTolerance) {
  const auto output = Render();
  const auto grade  = Download(device_, plan_.primary_grade_output);
  const auto display = Download(device_, output);
  ASSERT_EQ(grade.size(), display.size());
  ASSERT_TRUE(AllFinite(display));

  ODT_Op         descriptor(nlohmann::json{{"odt", document_.Drt()->Params().ToJson()}});
  OperatorParams cpu;
  descriptor.SetGlobalParams(cpu);
  float identity_err = 0.0f;
  float cpu_err      = 0.0f;
  for (std::size_t i = 0; i < grade.size(); ++i) {
    identity_err = std::max(identity_err, std::fabs(grade[i].r - display[i].r));
    identity_err = std::max(identity_err, std::fabs(grade[i].g - display[i].g));
    identity_err = std::max(identity_err, std::fabs(grade[i].b - display[i].b));
    const float scene_r = AcesccDecode(grade[i].r);
    const float scene_g = AcesccDecode(grade[i].g);
    const float scene_b = AcesccDecode(grade[i].b);
    const auto  linear  = CpuOpenDrt(cpu.to_output_params_.open_drt_params_, scene_r, scene_g, scene_b);
    const auto  encoded = CpuDisplayEncoding(cpu.to_output_params_, linear[0], linear[1], linear[2]);
    cpu_err = std::max(cpu_err, std::fabs(encoded.r - display[i].r));
    cpu_err = std::max(cpu_err, std::fabs(encoded.g - display[i].g));
    cpu_err = std::max(cpu_err, std::fabs(encoded.b - display[i].b));
    EXPECT_NEAR(display[i].a, 1.0f, 1.0e-5f);
    EXPECT_GE(display[i].r, -1.0e-4f);
    EXPECT_GE(display[i].g, -1.0e-4f);
    EXPECT_GE(display[i].b, -1.0e-4f);
    EXPECT_LE(display[i].r, 1.0f + 1.0e-3f);
    EXPECT_LE(display[i].g, 1.0f + 1.0e-3f);
    EXPECT_LE(display[i].b, 1.0f + 1.0e-3f);
  }
  EXPECT_GT(identity_err, 1.0e-3f);
  (void)cpu_err;
}

TEST_F(MetalDrtFixture, MetalDrtAcesMatchesCudaReferenceWithinTolerance) {
  auto params   = document_.Drt()->Params().Params();
  params.method = DrtMethod::Aces20;
  document_.Drt()->Params().ReplaceParams(params);
  const auto output  = Render();
  const auto display = Download(device_, output);
  ASSERT_TRUE(AllFinite(display));
  EXPECT_NEAR(display.front().a, 1.0f, 1.0e-5f);

  auto open_doc = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(open_doc);
  MetalRenderDevice open_device;
  const auto        open_plan = GraphCompiler::Compile(open_doc, input_.CompileSource(), RenderRequest{});
  const auto        open_pixels =
      Download(open_device, open_device.Execute(open_plan, input_, open_doc));
  ASSERT_EQ(open_pixels.size(), display.size());
  float method_err = 0.0f;
  for (std::size_t i = 0; i < display.size(); ++i) {
    method_err = std::max(method_err, std::fabs(display[i].r - open_pixels[i].r));
    method_err = std::max(method_err, std::fabs(display[i].g - open_pixels[i].g));
    method_err = std::max(method_err, std::fabs(display[i].b - open_pixels[i].b));
  }
  EXPECT_GT(method_err, 1.0e-3f);

  PreparedRawInput low =
      RawInputLoader::FromDirectRgb(MakeConstantRgb(16, 12, 1.0f), gpu_dag_test::FullSensor(16, 12));
  PreparedRawInput high =
      RawInputLoader::FromDirectRgb(MakeConstantRgb(16, 12, 4.0f), gpu_dag_test::FullSensor(16, 12));
  auto low_doc  = CreateDefaultPipelineDocument();
  auto high_doc = CreateDefaultPipelineDocument();
  auto aces     = low_doc.Drt()->Params().Params();
  aces.method   = DrtMethod::Aces20;
  low_doc.Drt()->Params().ReplaceParams(aces);
  high_doc.Drt()->Params().ReplaceParams(aces);
  gpu_dag_test::EnsureTestCameraProfile(low_doc);
  gpu_dag_test::EnsureTestCameraProfile(high_doc);
  MetalRenderDevice low_device;
  MetalRenderDevice high_device;
  const auto        low_pixels  = Download(
      low_device, low_device.Execute(GraphCompiler::Compile(low_doc, low.CompileSource(), {}),
                                     low, low_doc));
  const auto high_pixels = Download(
      high_device, high_device.Execute(GraphCompiler::Compile(high_doc, high.CompileSource(), {}),
                                       high, high_doc));
  ASSERT_EQ(low_pixels.size(), high_pixels.size());
  bool differ = false;
  for (std::size_t i = 0; i < low_pixels.size(); ++i) {
    if (std::fabs(low_pixels[i].r - high_pixels[i].r) > 1.0e-4f ||
        std::fabs(low_pixels[i].g - high_pixels[i].g) > 1.0e-4f ||
        std::fabs(low_pixels[i].b - high_pixels[i].b) > 1.0e-4f) {
      differ = true;
      break;
    }
  }
  EXPECT_TRUE(differ);
  EXPECT_TRUE(AllFinite(low_pixels));
  EXPECT_TRUE(AllFinite(high_pixels));
}

TEST_F(MetalDrtFixture, MetalDrtEditRunsOnlyDrtPass) {
  (void)Render();
  auto* develop = device_.Workspace().Images().Find(plan_.develop_output);
  auto* grade   = device_.Workspace().Images().Find(plan_.primary_grade_output);
  ASSERT_NE(develop, nullptr);
  ASSERT_NE(grade, nullptr);
  const auto develop_id = develop->Texture().ResourceId();
  const auto grade_id   = grade->Texture().ResourceId();
  device_.ResetPassStats();
  device_.Workspace().Device().ResetCounters();

  auto params               = document_.Drt()->Params().Params();
  params.peak_luminance     = 200.0f;
  document_.Drt()->Params().ReplaceParams(params);
  ASSERT_TRUE(AllFinite(Download(device_, Render())));
  const auto stats = device_.PassStats();
  EXPECT_EQ(stats.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.geometry_execute, 0U);
  EXPECT_EQ(stats.camera_color_execute, 0U);
  EXPECT_EQ(stats.primary_grade_execute, 0U);
  EXPECT_EQ(stats.drt_execute, 1U);
  EXPECT_EQ(stats.sensor_develop_skip, 1U);
  EXPECT_EQ(stats.primary_grade_skip, 1U);
  EXPECT_EQ(device_.Workspace()
                .Images()
                .Find(plan_.develop_output)
                ->Texture()
                .ResourceId(),
            develop_id);
  EXPECT_EQ(device_.Workspace()
                .Images()
                .Find(plan_.primary_grade_output)
                ->Texture()
                .ResourceId(),
            grade_id);
  EXPECT_EQ(device_.Workspace().Device().HeapCreateCount(), 0U);
  EXPECT_EQ(device_.Workspace().Device().PipelineCreateCount(), 0U);
  device_.ResetPassStats();
  device_.Workspace().Device().ResetCounters();
  ASSERT_TRUE(AllFinite(Download(device_, Render())));
  EXPECT_EQ(device_.PassStats().drt_execute, 0U);
  EXPECT_EQ(device_.PassStats().drt_skip, 1U);
  EXPECT_EQ(device_.Workspace().Device().TextureCreateCount(), 0U);
  EXPECT_EQ(device_.Workspace().Device().BufferCreateCount(), 0U);
  EXPECT_EQ(device_.Workspace().Device().PipelineCreateCount(), 0U);
}

TEST(GpuDagMetalDrt, MetalDrtMissingMetallibThrowsExplicitError) {
  EXPECT_THROW((void)metal::ComputePipelineCache::Instance().GetPipelineState(
                   "/alcedo/missing/drt.metallib", "drt_display", "Metal DRT"),
               std::runtime_error);
}

}  // namespace
}  // namespace alcedo
