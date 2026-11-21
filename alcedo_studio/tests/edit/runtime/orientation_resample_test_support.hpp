//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/graph_compiler.hpp"

namespace alcedo::gpu_dag_test {

/// Verify every RGBA pixel from the backend's affine geometry pass for preview and export.
template <class Device, class Develop, class Resample>
void VerifyExactOrientationPixels(Develop develop, Resample resample) {
  constexpr Extent2D source{5, 3};
  using Pixel = std::array<float, 4>;
  std::array<Pixel, 15> pixels{};
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const float label = static_cast<float>(i + 1);
    pixels[i]         = {label / 16.0f, label / 32.0f, label / 64.0f, 1.0f};
  }
  struct Case {
    FrameOrientation         orientation;
    Extent2D                 extent;
    std::array<unsigned, 15> labels;
  };
  constexpr Case cases[] = {
      {FrameOrientation::Identity, {5, 3}, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}},
      {FrameOrientation::Rotate90Clockwise,
       {3, 5},
       {11, 6, 1, 12, 7, 2, 13, 8, 3, 14, 9, 4, 15, 10, 5}},
      {FrameOrientation::Rotate180, {5, 3}, {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1}},
      {FrameOrientation::Rotate90Counterclockwise,
       {3, 5},
       {5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11}},
      {FrameOrientation::FlipHorizontal,
       {5, 3},
       {5, 4, 3, 2, 1, 10, 9, 8, 7, 6, 15, 14, 13, 12, 11}},
      {FrameOrientation::FlipVertical, {5, 3}, {11, 12, 13, 14, 15, 6, 7, 8, 9, 10, 1, 2, 3, 4, 5}},
      {FrameOrientation::Transpose, {3, 5}, {1, 6, 11, 2, 7, 12, 3, 8, 13, 4, 9, 14, 5, 10, 15}},
      {FrameOrientation::Transverse, {3, 5}, {15, 10, 5, 14, 9, 4, 13, 8, 3, 12, 7, 2, 11, 6, 1}},
  };
  const auto prepared = RawInputLoader::FromDirectRgb(MakeF32RgbaPlane(source.width, source.height),
                                                      FullSensor(source.width, source.height));
  auto       document = CreateDefaultPipelineDocument();
  EnsureTestCameraProfile(document);

  Device device;
  auto   plan = GraphCompiler::Compile(document, prepared.CompileSource(), {});
  for (const auto quality : {RenderQuality::Preview, RenderQuality::Export}) {
    for (const auto& c : cases) {
      SCOPED_TRACE(static_cast<int>(quality));
      SCOPED_TRACE(static_cast<int>(c.orientation));
      plan.geometry = ResolveRenderGeometry(MakeSourceGeometry(source, source),
                                            ImageGeometryParams{.orientation = c.orientation}, {},
                                            ResolutionRequest{.quality = quality}, {});
      plan.encode_geometry_resample = !IsIdentityResample(plan.geometry);
      device.BeginRender();
      develop(device, plan, prepared, document);
      auto& workspace = device.Workspace();
      auto* sensor    = workspace.Images().Find(plan.sensor_linear_output);
      ASSERT_NE(sensor, nullptr);
      workspace.Device().UploadTexture2D(sensor->Texture(), std::as_bytes(std::span(pixels)),
                                         device.CommandContext());
      resample(device, plan);
      device.EndRender();
      device.WaitIdle();

      const auto* output = workspace.Images().Find(plan.geometry_output);
      ASSERT_NE(output, nullptr);
      EXPECT_EQ(output->Texture().Width(), c.extent.width);
      EXPECT_EQ(output->Texture().Height(), c.extent.height);
      std::array<Pixel, pixels.size()> actual{};
      workspace.Device().DownloadTexture2D(
          output->Texture(), std::as_writable_bytes(std::span(actual)), device.CommandContext());
      for (std::size_t i = 0; i < actual.size(); ++i) {
        EXPECT_EQ(actual[i], pixels[c.labels[i] - 1]) << "output pixel " << i;
      }
    }
  }
}

}  // namespace alcedo::gpu_dag_test
