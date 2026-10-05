//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Raster input through the OpenCL develop graph: UploadRgb with LinearizeRaster, then DisplayToAp1
// (raster_image_input_plan.md, Phase R3).

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "edit/graph/develop_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/opencl/opencl_develop_pass.hpp"
#include "edit/runtime/opencl/opencl_renderer.hpp"
#include "opencl/opencl_runtime.hpp"
#include "raster_render_test_support.hpp"

namespace alcedo {
namespace {

using raster_render_test::Rgba;

class OpenClRasterDevelopTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!TryInitializeOpenClRuntime()) {
      GTEST_SKIP() << "No OpenCL device available.";
    }
    device_ = std::make_unique<OpenClRenderDevice>();
  }

  auto Download(const GraphValueId& id) -> std::vector<Rgba> {
    auto* lease = device_->Workspace().Images().Find(id);
    EXPECT_NE(lease, nullptr);
    if (lease == nullptr) {
      return {};
    }
    auto&             texture = lease->Texture();
    std::vector<Rgba> pixels(static_cast<std::size_t>(texture.Width()) * texture.Height());
    device_->Workspace().Device().DownloadTexture2D(
        texture,
        std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                             pixels.size() * sizeof(Rgba)),
        device_->CommandContext());
    return pixels;
  }

  auto RenderDevelop(const raster_render_test::RasterCase& c) -> std::vector<Rgba> {
    const auto plan = GraphCompiler::Compile(c.document, c.input.CompileSource(), RenderRequest{});
    device_->BeginRender();
    ExecuteOpenClDevelop(*device_, plan, c.input, c.document);
    ExecuteOpenClGeometryResample(*device_, plan);
    ExecuteOpenClDisplayToAp1(*device_, plan, c.input, c.document);
    device_->EndRender();
    device_->WaitIdle();
    auto pixels = Download(plan.develop_output);
    device_->PublishResults();
    return pixels;
  }

  std::unique_ptr<OpenClRenderDevice> device_;
};

TEST_F(OpenClRasterDevelopTest, RasterJpegRendersThroughDisplayToAp1WithoutCameraToAp1Pass) {
  auto       c    = raster_render_test::LoadCase("srgb_icc_8bit.jpg", RasterFileKind::Jpeg);
  const auto plan = GraphCompiler::Compile(c.document, c.input.CompileSource(), RenderRequest{});
  device_->ResetPassStats();
  (void)device_->Execute(plan, c.input, c.document);
  device_->WaitIdle();
  const auto& stats = device_->PassStats();
  EXPECT_EQ(stats.display_to_ap1_execute, 1u);
  EXPECT_EQ(stats.camera_color_execute, 0u);
  EXPECT_EQ(stats.drt_execute, 1u);
}

TEST_F(OpenClRasterDevelopTest, DevelopOutputMatchesHostEvaluationForEachRasterKind) {
  struct Case {
    const char*    name_;
    RasterFileKind kind_;
  };
  for (const auto& entry : {Case{"srgb_chunk.png", RasterFileKind::Png},
                            Case{"cicp_rec2020_pq_16bit.png", RasterFileKind::Png},
                            Case{"prophoto_icc_16bit.tif", RasterFileKind::Tiff},
                            Case{"lut_based_rgb_icc.tif", RasterFileKind::Tiff},
                            Case{"chromaticities_p3_half.exr", RasterFileKind::OpenExr},
                            Case{"alcedo_export_rec2020_hlg.tif", RasterFileKind::Tiff},
                            Case{"alcedo_export_upstream_rec2020_v4.tif", RasterFileKind::Tiff}}) {
    SCOPED_TRACE(entry.name_);
    const auto c = raster_render_test::LoadCase(entry.name_, entry.kind_);
    raster_render_test::ExpectAcesccNear(
        RenderDevelop(c), raster_render_test::HostReferenceDevelopOutput(c.input, c.description),
        entry.name_);
  }
}

TEST_F(OpenClRasterDevelopTest, RasterDevelopOutputIsCachedAcrossColorGradeEdits) {
  auto       c    = raster_render_test::LoadCase("srgb_icc_8bit.jpg", RasterFileKind::Jpeg);
  const auto plan = GraphCompiler::Compile(c.document, c.input.CompileSource(), RenderRequest{});
  device_->ResetPassStats();
  (void)device_->Execute(plan, c.input, c.document);
  auto* exposure = dynamic_cast<ExposureModel*>(
      c.document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(0.7f);
  (void)device_->Execute(plan, c.input, c.document);
  device_->WaitIdle();
  EXPECT_EQ(device_->PassStats().display_to_ap1_execute, 1u);
  EXPECT_EQ(device_->PassStats().display_to_ap1_skip, 1u);
}

TEST_F(OpenClRasterDevelopTest, InputProfileOverrideReRunsTheRasterDevelopChain) {
  auto       c    = raster_render_test::LoadCase("srgb_icc_8bit.jpg", RasterFileKind::Jpeg);
  const auto plan = GraphCompiler::Compile(c.document, c.input.CompileSource(), RenderRequest{});
  device_->ResetPassStats();
  (void)device_->Execute(plan, c.input, c.document);
  c.document.Develop()->Params().ApplyInputProfileUpdate(DevelopInputProfileUpdate{"adobe_rgb"});
  (void)device_->Execute(plan, c.input, c.document);
  device_->WaitIdle();
  EXPECT_EQ(device_->PassStats().sensor_develop_execute, 2u);
  EXPECT_EQ(device_->PassStats().display_to_ap1_execute, 2u);
  const auto expected =
      ResolveEffectiveRasterDescription(*c.document.Develop()->Params().RasterInput());
  raster_render_test::ExpectAcesccNear(
      RenderDevelop(c), raster_render_test::HostReferenceDevelopOutput(c.input, expected),
      "adobe_rgb override");
}

}  // namespace
}  // namespace alcedo
