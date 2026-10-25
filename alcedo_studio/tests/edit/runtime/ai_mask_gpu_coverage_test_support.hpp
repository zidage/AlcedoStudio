//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

// SM8.2 to SM8.4: device Feather and Expand of AI Mask coverage, checked against the independent
// host references of ai_mask_edge_reference.hpp. Each backend test file runs these checks on its
// AiMaskBackendHarness.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ai_mask_edge_reference.hpp"
#include "ai_mask_raster_test_support.hpp"
#include "edit/geometry/types.hpp"
#include "edit/mask/ai_mask_coverage_texture.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/runtime/ai_mask_coverage_work.hpp"

namespace alcedo::ai_mask_test {

/// Odd analysis size of the edge checks.
inline constexpr Extent2D kEdgeAnalysisExtent{67, 45};

/**
 * @brief Whole-image coverage with varied values: a block at the top-left image corner, a soft
 *        disk inside, a thin line along the right edge, and isolated texels at the bottom edge.
 */
inline auto               MakeEdgeTestImage(Extent2D extent) -> std::vector<std::uint8_t> {
  const int                 width  = static_cast<int>(extent.width);
  const int                 height = static_cast<int>(extent.height);
  std::vector<std::uint8_t> image(static_cast<std::size_t>(width) * height, 0);
  const auto                set = [&](int x, int y, int value) {
    image[static_cast<std::size_t>(y) * width + x] = static_cast<std::uint8_t>(value);
  };
  for (int y = 0; y < 7; ++y) {
    for (int x = 0; x < 9; ++x) {
      set(x, y, 40 + (x * 37 + y * 11) % 200);
    }
  }
  const double cx = width * 0.55;
  const double cy = height * 0.5;
  const double r  = height * 0.27;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy) / r;
      if (d < 1.0) {
        set(x, y, static_cast<int>(std::lround(std::min(1.0, (1.0 - d) * 3.0) * 255.0)));
      }
    }
  }
  for (int y = 5; y < height - 6; ++y) {
    set(width - 1, y, 90 + y);
  }
  set(width / 3, height - 1, 255);
  set(width / 3 + 4, height - 1, 128);
  return image;
}

/// Candidate read of @ref MakeEdgeTestImage.
inline auto MakeEdgeTestCandidate(Extent2D extent) -> AiMaskCoveragePixels {
  return std::make_shared<const std::vector<std::uint8_t>>(MakeEdgeTestImage(extent));
}

/**
 * @brief Stored asset at @p rect of an analysis image of @p analysis, cropped from
 *        @ref MakeEdgeTestImage like CropAiMaskCoverage: the texels on a rectangle side inside
 *        the image are 0.
 */
inline auto MakeEdgeTestAsset(Extent2D analysis, RectI rect) -> MaskAsset {
  const auto image = MakeEdgeTestImage(analysis);
  MaskAsset  asset;
  asset.descriptor.extent =
      Extent2D{static_cast<std::uint32_t>(rect.width), static_cast<std::uint32_t>(rect.height)};
  asset.descriptor.reference_bounds =
      NormalizedRect{static_cast<float>(rect.x) / static_cast<float>(analysis.width),
                     static_cast<float>(rect.y) / static_cast<float>(analysis.height),
                     static_cast<float>(rect.width) / static_cast<float>(analysis.width),
                     static_cast<float>(rect.height) / static_cast<float>(analysis.height)};
  asset.pixels = CropRect(image, static_cast<int>(analysis.width), rect);
  for (int y = 0; y < rect.height; ++y) {
    for (int x = 0; x < rect.width; ++x) {
      const bool inner_side =
          (x == 0 && rect.x > 0) || (y == 0 && rect.y > 0) ||
          (x == rect.width - 1 && rect.X1() < static_cast<int>(analysis.width)) ||
          (y == rect.height - 1 && rect.Y1() < static_cast<int>(analysis.height));
      if (inner_side) {
        asset.pixels[static_cast<std::size_t>(y) * rect.width + x] = 0;
      }
    }
  }
  asset.key = MakeMaskAssetKey(asset.descriptor, asset.pixels);
  return asset;
}

inline auto StoredSource(const MaskAsset& asset, Extent2D analysis, AiMaskEdgeAdjustment edge)
    -> AiSegmentationMaskSource {
  AiSegmentationMaskSource source;
  source.asset_key       = asset.key;
  source.descriptor      = asset.descriptor;
  source.analysis_extent = analysis;
  source.edge            = edge;
  return source;
}

/// Pixel rectangle of @p asset in the analysis image.
inline auto AssetRect(const MaskAsset& asset, Extent2D analysis) -> RectI {
  const auto rect = AiMaskAssetPixelRect(asset.descriptor, analysis);
  if (!rect.has_value()) {
    throw std::runtime_error("the test asset does not lie in the analysis image");
  }
  return *rect;
}

/// Independent reference of the stored @p asset after @p edge on the whole analysis image.
inline auto ReferenceWholeImage(const MaskAsset& asset, Extent2D analysis,
                                AiMaskEdgeAdjustment edge) -> std::vector<std::uint8_t> {
  const int width  = static_cast<int>(analysis.width);
  const int height = static_cast<int>(analysis.height);
  return ReferenceAiMaskEdgeOnImage(
      PlaceInImage(asset.pixels, AssetRect(asset, analysis), width, height), width, height, edge);
}

inline auto EdgeLabel(const AiMaskEdgeAdjustment& edge) -> std::string {
  return "feather " + std::to_string(edge.feather_px) + " expand " + std::to_string(edge.expand_px);
}

/// Give the workspace a reader with @p assets.
inline auto UseAssets(AiMaskBackendHarness& h, std::span<const MaskAsset> assets)
    -> std::shared_ptr<InMemoryMaskAssetReader> {
  auto reader = std::make_shared<InMemoryMaskAssetReader>();
  for (const auto& asset : assets) {
    reader->Put(asset);
  }
  h.set_reader(reader);
  return reader;
}

/// Expand alone: the alternating cross/square steps match the reference texel for texel.
inline void CheckGpuExpandMatchesAlternatingMorphologyExactly(AiMaskBackendHarness& h) {
  const auto analysis = kEdgeAnalysisExtent;
  const auto pixels   = MakeEdgeTestCandidate(analysis);
  const int  width    = static_cast<int>(analysis.width);
  const int  height   = static_cast<int>(analysis.height);
  for (const std::int32_t expand : {-50, -2, -1, 0, 1, 2, 3, 50}) {
    const AiMaskEdgeAdjustment edge{0.0f, expand};
    const auto                 actual = h.candidate_coverage(pixels, analysis, edge);
    EXPECT_EQ(actual.descriptor, AiMaskCandidateDescriptor(analysis)) << EdgeLabel(edge);
    EXPECT_EQ(actual.pixels, ReferenceAiMaskEdgeOnImage(*pixels, width, height, edge))
        << "candidate " << EdgeLabel(edge);
  }

  const auto                   asset = MakeEdgeTestAsset(analysis, RectI{21, 9, 31, 27});
  const std::vector<MaskAsset> assets{asset};
  (void)UseAssets(h, assets);
  for (const std::int32_t expand : {-50, -2, 2, 3, 50}) {
    const AiMaskEdgeAdjustment edge{0.0f, expand};
    const auto                 work   = MakeAiMaskCoverageWork(asset.descriptor, analysis, edge);
    const auto                 actual = h.stored_coverage(StoredSource(asset, analysis, edge));
    EXPECT_EQ(actual.descriptor, work.OutputDescriptor()) << EdgeLabel(edge);
    EXPECT_EQ(actual.pixels,
              CropRect(ReferenceWholeImage(asset, analysis, edge), width, work.output_rect))
        << "stored " << EdgeLabel(edge);
  }
}

/**
 * Feather, alone and after Expand: within one code value of the double-precision reference. The
 * largest difference to the host function of the runtime is printed separately.
 */
inline void CheckGpuFeatherMatchesIndependentGaussianWithinOneCode(AiMaskBackendHarness& h) {
  const auto analysis = kEdgeAnalysisExtent;
  const auto pixels   = MakeEdgeTestCandidate(analysis);
  const int  width    = static_cast<int>(analysis.width);
  const int  height   = static_cast<int>(analysis.height);
  MaskAsset  whole;
  whole.descriptor                   = AiMaskCandidateDescriptor(analysis);
  whole.pixels                       = *pixels;

  const auto                   asset = MakeEdgeTestAsset(analysis, RectI{21, 9, 31, 27});
  const std::vector<MaskAsset> assets{asset};
  (void)UseAssets(h, assets);

  int largest_reference_difference = 0;
  int largest_host_difference      = 0;
  for (const float feather : {0.5f, 5.0f, 50.0f}) {
    for (const std::int32_t expand : {0, -2, 2}) {
      const AiMaskEdgeAdjustment edge{feather, expand};
      const auto                 label     = EdgeLabel(edge);

      const auto                 candidate = h.candidate_coverage(pixels, analysis, edge);
      const auto expected = ReferenceAiMaskEdgeOnImage(*pixels, width, height, edge);
      ExpectR8Within1(candidate.pixels, expected, "candidate " + label);
      EXPECT_NE(candidate.pixels, *pixels) << "Feather changes the coverage: " << label;
      largest_reference_difference =
          std::max(largest_reference_difference, MaxR8Difference(candidate.pixels, expected));
      largest_host_difference =
          std::max(largest_host_difference,
                   MaxR8Difference(candidate.pixels,
                                   MakeAiMaskCoverageTexture(whole, analysis, edge).pixels));

      const auto work   = MakeAiMaskCoverageWork(asset.descriptor, analysis, edge);
      const auto stored = h.stored_coverage(StoredSource(asset, analysis, edge));
      EXPECT_EQ(stored.descriptor, work.OutputDescriptor()) << label;
      ExpectR8Within1(stored.pixels,
                      CropRect(ReferenceWholeImage(asset, analysis, edge), width, work.output_rect),
                      "stored " + label);
      largest_host_difference = std::max(
          largest_host_difference,
          MaxR8Difference(stored.pixels, MakeAiMaskCoverageTexture(asset, analysis, edge).pixels));
    }
  }
  std::cout << "[ measured ] largest difference to the double reference: "
            << largest_reference_difference
            << " code; to the host function: " << largest_host_difference << " code\n";
}

/**
 * Assets at the image corners and sides, and inside it: the output covers the margin of
 * AiMaskEdgeMargin clipped to the image, every value above 0 of the whole-image reference lies in
 * it, and the texels match the reference within one code value.
 */
inline void CheckGpuCoverageIncludesFullMarginAtImageBorders(AiMaskBackendHarness& h) {
  const auto                   analysis = kEdgeAnalysisExtent;
  const int                    width    = static_cast<int>(analysis.width);
  const int                    height   = static_cast<int>(analysis.height);
  // Top-left corner block, right side line, bottom side texels, interior disk.
  const std::vector<MaskAsset> assets{
      MakeEdgeTestAsset(analysis, RectI{0, 0, 10, 8}),
      MakeEdgeTestAsset(analysis, RectI{width - 2, 4, 2, height - 9}),
      MakeEdgeTestAsset(analysis, RectI{width / 3 - 1, height - 2, 7, 2}),
      MakeEdgeTestAsset(analysis, RectI{21, 9, 31, 27})};
  (void)UseAssets(h, assets);

  for (const auto& edge : {AiMaskEdgeAdjustment{2.0f, 3}, AiMaskEdgeAdjustment{5.0f, -2},
                           AiMaskEdgeAdjustment{50.0f, 50}}) {
    for (std::size_t a = 0; a < assets.size(); ++a) {
      const auto& asset  = assets[a];
      const auto  label  = "asset " + std::to_string(a) + " " + EdgeLabel(edge);
      const auto  rect   = AssetRect(asset, analysis);
      const auto  margin = AiMaskEdgeMargin(edge);
      const auto  work   = MakeAiMaskCoverageWork(asset.descriptor, analysis, edge);
      const RectI expected_rect{
          std::max(rect.x - margin, 0), std::max(rect.y - margin, 0),
          std::min(rect.X1() + margin, width) - std::max(rect.x - margin, 0),
          std::min(rect.Y1() + margin, height) - std::max(rect.y - margin, 0)};
      ASSERT_EQ(work.output_rect, expected_rect) << label;

      const auto whole   = ReferenceWholeImage(asset, analysis, edge);
      int        outside = 0;
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          const bool inside = x >= expected_rect.x && x < expected_rect.X1() &&
                              y >= expected_rect.y && y < expected_rect.Y1();
          if (!inside && whole[static_cast<std::size_t>(y) * width + x] > 0) {
            ++outside;
          }
        }
      }
      EXPECT_EQ(outside, 0) << "reference coverage outside the output: " << label;

      const auto actual = h.stored_coverage(StoredSource(asset, analysis, edge));
      EXPECT_EQ(actual.descriptor, work.OutputDescriptor()) << label;
      ExpectR8Within1(actual.pixels, CropRect(whole, width, expected_rect), label);
    }
  }
}

/// An edge change reuses the resident original upload: no asset read and no upload.
inline void CheckEdgeChangeDoesNotUploadOriginalAgain(AiMaskBackendHarness& h) {
  const auto analysis = kEdgeAnalysisExtent;
  const auto pixels   = MakeEdgeTestCandidate(analysis);
  (void)h.candidate_coverage(pixels, analysis, {});
  const auto uploads = h.cache_uploads();
  const auto work    = h.cache_edge_work();
  for (const auto& edge : {AiMaskEdgeAdjustment{1.0f, 0}, AiMaskEdgeAdjustment{3.0f, 2},
                           AiMaskEdgeAdjustment{0.0f, -1}, AiMaskEdgeAdjustment{50.0f, 50}}) {
    (void)h.candidate_coverage(pixels, analysis, edge);
  }
  (void)h.candidate_coverage(pixels, analysis, {});
  EXPECT_EQ(h.cache_uploads(), uploads) << "candidate edge changes upload nothing";
  EXPECT_EQ(h.cache_edge_work(), work + 4);

  // A committed Mask in the render: a Feather drag changes only the edge result.
  const auto asset  = MakeEllipseAsset(kAnalysisExtent, kAssetRect);
  const auto reader = AttachAiMask(h, asset, AiMaskEdgeAdjustment{1.0f, 0});
  h.compile({});
  h.render_mask();
  const auto stored_uploads = h.cache_uploads();
  for (const float feather : {2.0f, 4.5f, 8.0f}) {
    auto source = std::get<AiSegmentationMaskSource>(
        h.document().PrimaryGrade()->FindMask(kAiMaskId)->source);
    source.edge.feather_px = feather;
    h.document().PrimaryGrade()->ReplaceMaskSource(kAiMaskId, source);
    h.compile({});
    h.render_mask();
    const AiMaskEdgeAdjustment edge{feather, 0};
    ExpectR8Within1(h.download_mask(), ExpectedAiMaskR8(asset, edge, h.plan(), false, 1.0),
                    "render " + EdgeLabel(edge));
  }
  EXPECT_EQ(h.cache_uploads(), stored_uploads);
  EXPECT_EQ(reader->ReadCount(), 1);
}

/**
 * A failed allocation of the edge output or of a work texture reports the error, and the published
 * coverage stays current with its content; a retry computes the requested result.
 */
inline void CheckFailedEdgeWorkKeepsPublishedCoverage(AiMaskBackendHarness& h) {
  const auto                 analysis = kEdgeAnalysisExtent;
  const auto                 pixels   = MakeEdgeTestCandidate(analysis);
  const AiMaskEdgeAdjustment shown{2.0f, 0};
  const auto                 published = h.candidate_coverage(pixels, analysis, shown);
  const auto                 entries   = h.cache_entries();
  const auto                 bytes     = h.cache_used_bytes();
  const auto                 uploads   = h.cache_uploads();
  const auto                 work      = h.cache_edge_work();

  // Expand 3 with Feather needs two new R8 work textures: fail the output, then the first of them.
  const AiMaskEdgeAdjustment requested{4.0f, 3};
  for (const std::uint32_t allocations_to_pass : {0u, 1u}) {
    h.fail_texture_allocation_after(allocations_to_pass);
    try {
      (void)h.candidate_coverage(pixels, analysis, requested);
      ADD_FAILURE() << "the allocation failure was not reported: " << allocations_to_pass;
    } catch (const std::runtime_error& error) {
      const std::string message = error.what();
      EXPECT_NE(message.find("AI Mask coverage: allocating the 67 x 45 R8 texture failed"),
                std::string::npos)
          << message;
    }
    EXPECT_EQ(h.cache_entries(), entries) << allocations_to_pass;
    EXPECT_EQ(h.cache_used_bytes(), bytes) << allocations_to_pass;
  }

  const auto kept = h.candidate_coverage(pixels, analysis, shown);
  EXPECT_EQ(kept.pixels, published.pixels);
  EXPECT_EQ(h.cache_edge_work(), work) << "the published result is still current";
  EXPECT_EQ(h.cache_uploads(), uploads);

  const auto retried = h.candidate_coverage(pixels, analysis, requested);
  ExpectR8Within1(retried.pixels,
                  ReferenceAiMaskEdgeOnImage(*pixels, static_cast<int>(analysis.width),
                                             static_cast<int>(analysis.height), requested),
                  "retry");
  EXPECT_EQ(h.cache_edge_work(), work + 1);
  EXPECT_EQ(h.cache_uploads(), uploads);
}

}  // namespace alcedo::ai_mask_test
