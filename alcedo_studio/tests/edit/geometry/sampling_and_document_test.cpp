//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/geometry/texture_sampling_plan.hpp"
#include "edit/graph/pipeline_document.hpp"

namespace alcedo {
namespace {

auto FileContainsToken(const std::filesystem::path& path, const char* token) -> bool {
  std::ifstream input(path);
  std::string   line;
  while (std::getline(input, line)) {
    if (line.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void ScanDirectoryForToken(const std::filesystem::path& root, const char* token,
                           std::vector<std::string>* hits) {
  if (!std::filesystem::exists(root)) {
    hits->push_back("missing directory: " + root.string());
    return;
  }
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".hpp") {
      continue;
    }
    if (FileContainsToken(entry.path(), token)) {
      hits->push_back(entry.path().filename().string() + " " + token);
    }
  }
}

}  // namespace

TEST(GpuDagGeometry, RasterMaskSamplingMapsSameReferencePointAtQuarterAndFullPreview) {
  const auto source = MakeSourceGeometry({200, 100}, {200, 100});
  const NormalizedRect bounds{0.10f, 0.10f, 0.80f, 0.80f};
  const Vector2        ref_center{0.40f * 200.0f, 0.30f * 100.0f};

  ResolutionRequest full_res;
  full_res.render_scale = 1.0f;
  ResolutionRequest quarter_res;
  quarter_res.render_scale = 0.25f;

  const auto g_full    = ResolveRenderGeometry(source, {}, {}, full_res, {});
  const auto g_quarter = ResolveRenderGeometry(source, {}, {}, quarter_res, {});
  const auto plan_full =
      MakeRasterMaskSamplingPlan(g_full, bounds, Extent2D{256, 256});
  const auto plan_quarter =
      MakeRasterMaskSamplingPlan(g_quarter, bounds, Extent2D{256, 256});

  const auto uv_full = TransformPoint(
      plan_full.render_to_texture_uv, TransformPoint(g_full.reference_to_render, ref_center));
  const auto uv_quarter = TransformPoint(
      plan_quarter.render_to_texture_uv, TransformPoint(g_quarter.reference_to_render, ref_center));

  EXPECT_NEAR(uv_full.x, 0.375f, 1.0e-5f);
  EXPECT_NEAR(uv_full.y, 0.25f, 1.0e-5f);
  EXPECT_NEAR(uv_quarter.x, uv_full.x, 1.0e-5f);
  EXPECT_NEAR(uv_quarter.y, uv_full.y, 1.0e-5f);
}

TEST(GpuDagGeometry, OddImageDimensionsUseOneRoundingResultAcrossImageMaskAndLlf) {
  ImageGeometryParams image;
  image.crop_rect = NormalizedRect{0.10f, 0.20f, 0.50f, 0.40f};
  const auto source = MakeSourceGeometry({1001, 667}, {1001, 667});
  const auto first  = ResolveRenderGeometry(source, image, {}, {}, {});
  const auto second = ResolveRenderGeometry(source, image, {}, {}, {});
  EXPECT_EQ(first.render_extent, second.render_extent);
  EXPECT_EQ(first.edit_extent, second.edit_extent);
  EXPECT_EQ(first.required_decoded_region, second.required_decoded_region);
  EXPECT_EQ(first.required_reference_region, second.required_reference_region);
  EXPECT_EQ(first.required_decoded_region, first.required_reference_region);

  SamplingFootprint llf_footprint;
  llf_footprint.requires_full_reference = true;
  const auto with_llf = ResolveRenderGeometry(source, image, {}, {}, llf_footprint);
  EXPECT_EQ(with_llf.render_extent, first.render_extent);
  EXPECT_EQ(with_llf.required_decoded_region,
            (RectI{0, 0, 1001, 667}));

  const auto mask = MakeRasterMaskSamplingPlan(first, NormalizedRect{}, Extent2D{512, 512});
  const auto llf  = MakeLlfSamplingPlan(first, Extent2D{128, 128});
  const auto center = PixelCenter(3, 5);
  const auto ref    = TransformPoint(first.render_to_reference, center);
  const auto n      = NormalizedFromPixelCenter(ref, first.full_reference_extent);
  const auto uv_m   = TransformPoint(mask.render_to_texture_uv, center);
  const auto uv_l   = TransformPoint(llf.render_to_texture_uv, center);
  EXPECT_NEAR(uv_m.x, n.x, 1.0e-5f);
  EXPECT_NEAR(uv_m.y, n.y, 1.0e-5f);
  EXPECT_NEAR(uv_l.x, n.x, 1.0e-5f);
  EXPECT_NEAR(uv_l.y, n.y, 1.0e-5f);
}

TEST(GpuDagGeometry, DefaultPipelineDocumentHasNoGeometryNode) {
  const auto document = CreateDefaultPipelineDocument();
  ASSERT_EQ(document.Graph().NodeCount(), 3u);
  ASSERT_NE(document.Develop(), nullptr);
  ASSERT_NE(document.PrimaryGrade(), nullptr);
  ASSERT_NE(document.Drt(), nullptr);
  const auto json = document.ToJson();
  const auto text = json.dump();
  EXPECT_TRUE(json.contains("geometry"));
  EXPECT_TRUE(json["geometry"].contains("crop_rect"));
  EXPECT_TRUE(json["geometry"].contains("rotation_degrees"));
  EXPECT_FALSE(text.find("viewport") != std::string::npos);
  EXPECT_FALSE(text.find("render_scale") != std::string::npos);
  EXPECT_FALSE(text.find("\"roi_") != std::string::npos);
}

TEST(GpuDagGeometry, OperatorParamPayloadsContainNoRoiFields) {
  std::vector<std::string> hits;
  ScanDirectoryForToken(std::filesystem::path{ALCEDO_MODEL_HEADER_ROOT}, "roi_", &hits);
  EXPECT_TRUE(hits.empty()) << (hits.empty() ? "" : hits.front());
}

TEST(GpuDagGeometry, GeometryHeadersDoNotIncludeGpuOrImageBuffer) {
  std::vector<std::string> hits;
  const auto               root = std::filesystem::path{ALCEDO_GEOMETRY_HEADER_ROOT};
  if (!std::filesystem::exists(root)) {
    FAIL() << "missing geometry header root";
  }
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".hpp") {
      continue;
    }
    const char* tokens[] = {"image_buffer.hpp", "cuda.h", "cuda_runtime", "OpenCL", "Metal/"};
    for (const char* token : tokens) {
      if (FileContainsToken(entry.path(), token)) {
        hits.push_back(entry.path().filename().string() + " " + token);
      }
    }
  }
  EXPECT_TRUE(hits.empty()) << (hits.empty() ? "" : hits.front());
}

}  // namespace alcedo
