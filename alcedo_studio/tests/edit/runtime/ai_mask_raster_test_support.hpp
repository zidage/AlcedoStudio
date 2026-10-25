//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "edit/geometry/render_request.hpp"
#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/ai_mask_coverage_texture.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/runtime/ai_mask_coverage_work.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/gpu_node_pass_stats.hpp"
#include "edit/runtime/mask_asset_reader.hpp"
#include "multi_mask_runtime_test_support.hpp"

namespace alcedo::ai_mask_test {

/**
 * @brief Host reference of the raster MaskEvaluate rule (SM6 R7) in double precision.
 *
 * Separate code from the backend kernels: it maps each sample through render_to_reference and the
 * texture bounds itself, and it does not use the sampling plan. Output is the effective R8
 * coverage at @p geometry.render_extent.
 */
inline auto EvaluateAiMaskR8(const AiMaskCoverageTexture&  texture,
                             const ResolvedRenderGeometry& geometry, bool invert, double opacity)
    -> std::vector<std::uint8_t> {
  const auto&  m       = geometry.render_to_reference.m;
  const auto&  bounds  = texture.descriptor.reference_bounds;
  const double full_w  = geometry.full_reference_extent.width;
  const double full_h  = geometry.full_reference_extent.height;
  const double tex_w   = texture.descriptor.extent.width;
  const double tex_h   = texture.descriptor.extent.height;
  // Texels per render pixel along each render axis.
  const double du_dx   = m[0] / (full_w * bounds.w) * tex_w;
  const double dv_dx   = m[3] / (full_h * bounds.h) * tex_h;
  const double du_dy   = m[1] / (full_w * bounds.w) * tex_w;
  const double dv_dy   = m[4] / (full_h * bounds.h) * tex_h;
  const double rho     = std::max(std::hypot(du_dx, dv_dx), std::hypot(du_dy, dv_dy));
  const int    samples = std::clamp(static_cast<int>(std::ceil(rho - 1.0e-3)), 1, 8);

  const auto   texel   = [&](int x, int y) -> double {
    x = std::clamp(x, 0, static_cast<int>(tex_w) - 1);
    y = std::clamp(y, 0, static_cast<int>(tex_h) - 1);
    return texture.pixels[static_cast<std::size_t>(y) * texture.descriptor.extent.width + x] /
           255.0;
  };

  const auto                width  = geometry.render_extent.width;
  const auto                height = geometry.render_extent.height;
  std::vector<std::uint8_t> result(static_cast<std::size_t>(width) * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      double sum = 0.0;
      for (int j = 0; j < samples; ++j) {
        for (int i = 0; i < samples; ++i) {
          const double px = x + (i + 0.5) / samples;
          const double py = y + (j + 0.5) / samples;
          const double rx = m[0] * px + m[1] * py + m[2];
          const double ry = m[3] * px + m[4] * py + m[5];
          const double u  = (rx / full_w - bounds.x) / bounds.w;
          const double v  = (ry / full_h - bounds.y) / bounds.h;
          if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0) {
            continue;
          }
          const double tx     = u * tex_w - 0.5;
          const double ty     = v * tex_h - 0.5;
          const double fx     = tx - std::floor(tx);
          const double fy     = ty - std::floor(ty);
          const int    x0     = static_cast<int>(std::floor(tx));
          const int    y0     = static_cast<int>(std::floor(ty));
          const double top    = texel(x0, y0) * (1.0 - fx) + texel(x0 + 1, y0) * fx;
          const double bottom = texel(x0, y0 + 1) * (1.0 - fx) + texel(x0 + 1, y0 + 1) * fx;
          sum += top * (1.0 - fy) + bottom * fy;
        }
      }
      double value = sum / (samples * samples);
      if (invert) {
        value = 1.0 - value;
      }
      value = std::clamp(value * opacity, 0.0, 1.0);
      result[static_cast<std::size_t>(y) * width + x] =
          static_cast<std::uint8_t>(std::floor(value * 255.0 + 0.5));
    }
  }
  return result;
}

/// In-memory Mask asset reader for backend tests; counts reads.
class InMemoryMaskAssetReader final : public MaskAssetReader {
 public:
  void Put(MaskAsset asset) {
    const std::string key{asset.key.Value()};
    assets_[key] = std::move(asset);
  }

  [[nodiscard]] auto Read(const MaskAssetKey& key) const -> MaskAsset override {
    ++read_count_;
    const auto it = assets_.find(std::string{key.Value()});
    if (it == assets_.end()) {
      throw std::runtime_error("Mask asset " + std::string{key.Value()} +
                               " is missing from the project");
    }
    return it->second;
  }

  [[nodiscard]] auto ReadCount() const -> int { return read_count_; }

 private:
  std::unordered_map<std::string, MaskAsset> assets_;
  mutable int                                read_count_ = 0;
};

/**
 * @brief Test asset in an analysis image of @p analysis: a filled ellipse with a soft rim inside
 *        the asset rectangle, zero border of one pixel.
 */
inline auto MakeEllipseAsset(Extent2D analysis, RectI rect) -> MaskAsset {
  MaskAsset asset;
  asset.descriptor.extent =
      Extent2D{static_cast<std::uint32_t>(rect.width), static_cast<std::uint32_t>(rect.height)};
  asset.descriptor.reference_bounds =
      NormalizedRect{static_cast<float>(rect.x) / static_cast<float>(analysis.width),
                     static_cast<float>(rect.y) / static_cast<float>(analysis.height),
                     static_cast<float>(rect.width) / static_cast<float>(analysis.width),
                     static_cast<float>(rect.height) / static_cast<float>(analysis.height)};
  const double cx = rect.width / 2.0;
  const double cy = rect.height / 2.0;
  const double rx = rect.width / 2.0 - 1.5;
  const double ry = rect.height / 2.0 - 1.5;
  for (int y = 0; y < rect.height; ++y) {
    for (int x = 0; x < rect.width; ++x) {
      const double dx     = (x + 0.5 - cx) / rx;
      const double dy     = (y + 0.5 - cy) / ry;
      const double r      = std::sqrt(dx * dx + dy * dy);
      const double v      = std::clamp((1.0 - r) * 4.0, 0.0, 1.0);
      const bool   border = x == 0 || y == 0 || x == rect.width - 1 || y == rect.height - 1;
      asset.pixels.push_back(border ? 0 : static_cast<std::uint8_t>(std::lround(v * 255.0)));
    }
  }
  asset.key = MakeMaskAssetKey(asset.descriptor, asset.pixels);
  return asset;
}

/// AI Mask over @p asset in an analysis image of @p analysis.
inline auto MakeAiMask(const MaskAsset& asset, Extent2D analysis, std::string id,
                       AiMaskEdgeAdjustment edge = {}) -> MaskModel {
  AiSegmentationMaskSource source;
  source.asset_key       = asset.key;
  source.descriptor      = asset.descriptor;
  source.analysis_extent = analysis;
  source.edge            = edge;
  source.recipe.components.push_back({AiMaskComponentOperation::Add, AiMaskTaskComponent{}});
  source.model_set_id         = "sam2.1-tiny";
  source.geometry_fingerprint = std::string(32, '0');
  MaskModel mask;
  mask.id           = MaskId{std::move(id)};
  mask.display_name = "AI Mask";
  mask.source       = std::move(source);
  return mask;
}

/// Result of one coverage acquisition in coverage-only work, downloaded after the wait.
struct CandidateCoverageDownload {
  MaskAssetDescriptor       descriptor{};
  std::vector<std::uint8_t> pixels;
};

/// Download the texture of @p read after the device is idle.
template <class Device, class Read>
auto DownloadCoverageRead(Device& device, const Read& read) -> CandidateCoverageDownload {
  CandidateCoverageDownload download;
  download.descriptor = read->descriptor;
  download.pixels.resize(read->texture.Bytes());
  device.Workspace().Device().DownloadTexture2D(
      read->texture,
      std::span<std::byte>(reinterpret_cast<std::byte*>(download.pixels.data()),
                           download.pixels.size()),
      device.CommandContext());
  return download;
}

/**
 * @brief Coverage-only work on @p device: BeginCoverageWork, the candidate acquisition,
 *        EndCoverageWork, WaitIdle, then a download of the acquired texture. Cancels the work and
 *        rethrows on an error.
 */
template <class Device>
auto AcquireCandidateInCoverageWork(Device& device, const AiMaskCoveragePixels& pixels,
                                    Extent2D analysis, AiMaskEdgeAdjustment edge)
    -> CandidateCoverageDownload {
  device.BeginCoverageWork();
  const auto read = [&] {
    try {
      return device.Workspace().AcquireAiMaskCandidateCoverage(pixels, analysis, edge,
                                                               device.CommandContext());
    } catch (...) {
      device.CancelCoverageWork();
      throw;
    }
  }();
  device.EndCoverageWork();
  device.WaitIdle();
  return DownloadCoverageRead(device, read);
}

/**
 * @brief Coverage-only work on @p device that acquires the committed AI Mask @p source through the
 *        workspace reader, then downloads the texture. Cancels the work and rethrows on an error.
 */
template <class Device>
auto AcquireStoredInCoverageWork(Device& device, const AiSegmentationMaskSource& source)
    -> CandidateCoverageDownload {
  device.BeginCoverageWork();
  const auto read = [&] {
    try {
      return device.Workspace().AcquireAiMaskCoverage(source, device.CommandContext());
    } catch (...) {
      device.CancelCoverageWork();
      throw;
    }
  }();
  device.EndCoverageWork();
  device.WaitIdle();
  return DownloadCoverageRead(device, read);
}

/// Resource id of each published photo result of @p plan, or 0 when the result is not published.
template <class Device>
auto PublishedPhotoResultIds(Device& device, const ExecutionPlan& plan)
    -> std::vector<std::uint64_t> {
  std::vector<GraphValueId> ids{plan.sensor_linear_output, plan.geometry_output,
                                plan.display_output};
  if (const auto* grade = plan.FirstGrade(); grade != nullptr) {
    ids.push_back(grade->mask_output);
    ids.push_back(grade->scene_output);
  }
  std::vector<std::uint64_t> result;
  for (const auto& id : ids) {
    const auto* lease = device.Workspace().Images().Find(id);
    result.push_back(lease == nullptr ? 0 : lease->Texture().ResourceId());
  }
  return result;
}

/**
 * @brief The operations of one backend mask fixture that the shared AI Mask checks use.
 *
 * Each backend test file builds it from its fixture. @ref render_mask runs BeginRender, the mask
 * passes of every Color Grade, EndRender, and WaitIdle; on an error it cancels the render and
 * rethrows.
 */
struct AiMaskBackendHarness {
  std::function<PipelineDocument&()>                          document;
  std::function<void(RenderRequest)>                          compile;
  std::function<const ExecutionPlan&()>                       plan;
  std::function<void()>                                       render_mask;
  std::function<std::vector<std::uint8_t>()>                  download_mask;
  std::function<void(std::shared_ptr<const MaskAssetReader>)> set_reader;
  std::function<std::size_t()>                                cache_entries;
  std::function<std::uint64_t()>                              cache_uploads;
  /// WaitIdle, then BasicRenderWorkspace::ReleaseSessionResources.
  std::function<void()>                                       release_session;
  /// Execute the whole plan, publish its results, and WaitIdle.
  std::function<void()>                                       execute_plan;
  /// @ref PublishedPhotoResultIds of the current plan.
  std::function<std::vector<std::uint64_t>()>                 photo_result_ids;
  std::function<std::size_t()>                                published_count;
  std::function<std::uint64_t()>                              texture_allocations;
  std::function<GpuNodePassStats()>                           pass_stats;
  std::function<void()>                                       reset_pass_stats;
  /// @ref AcquireCandidateInCoverageWork on the fixture device.
  std::function<CandidateCoverageDownload(const AiMaskCoveragePixels&, Extent2D,
                                          AiMaskEdgeAdjustment)>
      candidate_coverage;
  /// @ref AcquireStoredInCoverageWork on the fixture device.
  std::function<CandidateCoverageDownload(const AiSegmentationMaskSource&)> stored_coverage;
  /// MaskCoverageTextureCache::EdgeWorkCount and UsedBytes.
  std::function<std::uint64_t()>                                            cache_edge_work;
  std::function<std::size_t()>                                              cache_used_bytes;
  /// Fail the texture allocation after @p allocations_to_pass more allocations.
  std::function<void(std::uint32_t)> fail_texture_allocation_after;
};

/// Analysis image of the checks: the full reference of a 64 x 48 test source.
inline constexpr Extent2D kAnalysisExtent{64, 48};
inline const RectI        kAssetRect{18, 10, 28, 22};
inline const MaskId       kAiMaskId{"mask.ai"};

inline void               ExpectR8Within1(std::span<const std::uint8_t> actual,
                                          std::span<const std::uint8_t> expected, const std::string& label) {
  ASSERT_EQ(actual.size(), expected.size()) << label;
  int failures = 0;
  for (std::size_t i = 0; i < actual.size() && failures < 8; ++i) {
    if (std::abs(static_cast<int>(actual[i]) - static_cast<int>(expected[i])) > 1) {
      ADD_FAILURE() << label << ": index " << i << " actual " << int(actual[i]) << " expected "
                    << int(expected[i]);
      ++failures;
    }
  }
}

/// Add the AI Mask of @p asset to the primary Grade and give the workspace a reader with it.
inline auto AttachAiMask(AiMaskBackendHarness& h, const MaskAsset& asset,
                         AiMaskEdgeAdjustment edge = {})
    -> std::shared_ptr<InMemoryMaskAssetReader> {
  auto reader = std::make_shared<InMemoryMaskAssetReader>();
  reader->Put(asset);
  h.set_reader(reader);
  grade_mask_test::AddMask(
      *h.document().PrimaryGrade(),
      MakeAiMask(asset, kAnalysisExtent, std::string{kAiMaskId.Value()}, edge));
  h.document().MarkTopologyChanged();
  return reader;
}

inline auto ExpectedAiMaskR8(const MaskAsset& asset, AiMaskEdgeAdjustment edge,
                             const ExecutionPlan& plan, bool invert, double opacity)
    -> std::vector<std::uint8_t> {
  return EvaluateAiMaskR8(MakeAiMaskCoverageTexture(asset, kAnalysisExtent, edge), plan.geometry,
                          invert, opacity);
}

inline void CheckAiMaskMatchesReferenceAtFullAndPreviewScale(AiMaskBackendHarness& h) {
  const auto asset = MakeEllipseAsset(kAnalysisExtent, kAssetRect);
  (void)AttachAiMask(h, asset);
  for (const float scale : {1.0f, 0.5f, 0.25f}) {
    RenderRequest request;
    request.resolution.render_scale = scale;
    h.compile(request);
    h.render_mask();
    const auto expected = ExpectedAiMaskR8(asset, {}, h.plan(), false, 1.0);
    const auto actual   = h.download_mask();
    ExpectR8Within1(actual, expected, "render scale " + std::to_string(scale));
    EXPECT_GT(*std::max_element(actual.begin(), actual.end()), 200) << scale;
  }
}

inline void CheckAiMaskFollowsCropAndRotation(AiMaskBackendHarness& h) {
  const auto asset = MakeEllipseAsset(kAnalysisExtent, kAssetRect);
  (void)AttachAiMask(h, asset);
  h.document().Geometry().SetCropRect({0.1f, 0.15f, 0.8f, 0.7f});
  h.document().Geometry().SetRotationDegrees(30.0f);
  h.compile({});
  h.render_mask();
  ExpectR8Within1(h.download_mask(), ExpectedAiMaskR8(asset, {}, h.plan(), false, 1.0),
                  "crop and rotation");
}

inline void CheckAiMaskOutsideTextureBoundsIsZero(AiMaskBackendHarness& h) {
  const auto asset = MakeEllipseAsset(kAnalysisExtent, kAssetRect);
  (void)AttachAiMask(h, asset);
  h.compile({});
  h.render_mask();
  const auto plain = h.download_mask();
  const auto width = h.plan().geometry.render_extent.width;
  // Pixel (2, 2) and the last pixel map outside the asset bounds.
  EXPECT_EQ(plain[2 * width + 2], 0);
  EXPECT_EQ(plain.back(), 0);
  h.document().PrimaryGrade()->SetMaskInvert(kAiMaskId, true);
  h.compile({});
  h.render_mask();
  const auto inverted = h.download_mask();
  EXPECT_EQ(inverted[2 * width + 2], 255);
  EXPECT_EQ(inverted.back(), 255);
}

inline void CheckAiMaskInvertAndOpacityApplyBeforeUnion(AiMaskBackendHarness& h) {
  const auto asset = MakeEllipseAsset(kAnalysisExtent, kAssetRect);
  (void)AttachAiMask(h, asset);
  h.document().PrimaryGrade()->SetMaskInvert(kAiMaskId, true);
  h.document().PrimaryGrade()->SetMaskOpacity(kAiMaskId, 0.5f);
  RadialMaskSource radial;
  radial.center_x        = 0.3f;
  radial.center_y        = 0.4f;
  radial.major_radius    = 0.2f;
  radial.minor_radius    = 0.15f;
  const auto radial_mask = grade_mask_test::MakeRadialMask(MaskId{"mask.radial"}, radial);
  grade_mask_test::AddMask(*h.document().PrimaryGrade(), radial_mask);
  h.document().MarkTopologyChanged();
  h.compile({});
  h.render_mask();

  const auto                   ai = ExpectedAiMaskR8(asset, {}, h.plan(), true, 0.5);
  const std::vector<MaskModel> radial_only{radial_mask};
  const auto radial_r8 = multi_mask_test::EvaluateEnabledUnionR8(radial_only, h.plan().geometry);
  std::vector<std::uint8_t> expected(ai.size());
  for (std::size_t i = 0; i < ai.size(); ++i) {
    expected[i] = std::max(ai[i], radial_r8[i]);
  }
  ExpectR8Within1(h.download_mask(), expected, "AI and Radial union");
}

inline void CheckAiMaskFeatherAndExpandChangeCoverage(AiMaskBackendHarness& h) {
  const auto                 asset = MakeEllipseAsset(kAnalysisExtent, kAssetRect);
  const AiMaskEdgeAdjustment edge{2.0f, 2};
  (void)AttachAiMask(h, asset, edge);
  h.compile({});
  h.render_mask();
  const auto actual = h.download_mask();
  ExpectR8Within1(actual, ExpectedAiMaskR8(asset, edge, h.plan(), false, 1.0),
                  "feather and expand");
  EXPECT_NE(actual, ExpectedAiMaskR8(asset, {}, h.plan(), false, 1.0));
}

/// The asset is read and uploaded once; an edge change adds only a device edge result (SM8.2).
inline void CheckAiMaskCoverageTextureUploadsOncePerAsset(AiMaskBackendHarness& h) {
  const auto asset  = MakeEllipseAsset(kAnalysisExtent, kAssetRect);
  const auto reader = AttachAiMask(h, asset);
  h.compile({});
  h.render_mask();
  h.render_mask();
  EXPECT_EQ(h.cache_uploads(), 1u);
  EXPECT_EQ(h.cache_entries(), 1u);
  EXPECT_EQ(reader->ReadCount(), 1);

  auto source =
      std::get<AiSegmentationMaskSource>(h.document().PrimaryGrade()->FindMask(kAiMaskId)->source);
  source.edge.feather_px = 1.5f;
  h.document().PrimaryGrade()->ReplaceMaskSource(kAiMaskId, source);
  h.compile({});
  h.render_mask();
  EXPECT_EQ(h.cache_uploads(), 1u);
  EXPECT_EQ(h.cache_entries(), 2u);
  EXPECT_EQ(reader->ReadCount(), 1);

  h.release_session();
  EXPECT_EQ(h.cache_entries(), 0u);
}

/// Session-size candidate coverage: a rectangle of 200 with a ramp along its top row.
inline auto MakeCandidatePixels(Extent2D analysis) -> AiMaskCoveragePixels {
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(analysis.width) * analysis.height, 0);
  for (std::uint32_t y = 10; y < 30; ++y) {
    for (std::uint32_t x = 12; x < 40; ++x) {
      pixels[static_cast<std::size_t>(y) * analysis.width + x] =
          y == 10 ? static_cast<std::uint8_t>(x * 6) : 200;
    }
  }
  return std::make_shared<const std::vector<std::uint8_t>>(std::move(pixels));
}

/**
 * SM8.1: coverage-only work uploads the candidate on the device submission path and leaves the
 * published photo results, the texture pool, and the next render unchanged. The edge result
 * matches the host function within one code value (SM8.2 device Feather).
 */
inline void CheckCoverageOnlyWorkKeepsPhotoResults(AiMaskBackendHarness& h) {
  const auto asset = MakeEllipseAsset(kAnalysisExtent, kAssetRect);
  (void)AttachAiMask(h, asset);
  h.compile({});
  h.execute_plan();
  // Control: the pass counters of a repeated render without coverage work.
  h.reset_pass_stats();
  h.execute_plan();
  const auto control            = h.pass_stats();
  const auto results_before     = h.photo_result_ids();
  const auto published_before   = h.published_count();
  const auto allocations_before = h.texture_allocations();
  ASSERT_EQ(control.sensor_develop_skip, 1U);
  // Order of PublishedPhotoResultIds: Develop, Geometry, display, Mask, Color Grade scene. The
  // Color Grade scene result lives in the scene-work pair, not in the published images.
  ASSERT_EQ(results_before.size(), 5U);
  ASSERT_NE(results_before[2], 0U) << "the display result is not published";
  ASSERT_NE(results_before[3], 0U) << "the Mask result is not published";

  // The original candidate coverage: the read is the uploaded session pixels.
  const auto candidate = MakeCandidatePixels(kAnalysisExtent);
  const auto original  = h.candidate_coverage(candidate, kAnalysisExtent, {});
  EXPECT_EQ(original.descriptor, AiMaskCandidateDescriptor(kAnalysisExtent));
  EXPECT_EQ(original.pixels, *candidate);
  // An edge result of the same allocation.
  const AiMaskEdgeAdjustment edge{2.0f, 1};
  const auto                 adjusted = h.candidate_coverage(candidate, kAnalysisExtent, edge);
  MaskAsset                  whole_image;
  whole_image.descriptor = AiMaskCandidateDescriptor(kAnalysisExtent);
  whole_image.pixels     = *candidate;
  const auto expected    = MakeAiMaskCoverageTexture(whole_image, kAnalysisExtent, edge);
  EXPECT_EQ(adjusted.descriptor, expected.descriptor);
  ExpectR8Within1(adjusted.pixels, expected.pixels, "candidate edge result");

  EXPECT_EQ(h.photo_result_ids(), results_before);
  EXPECT_EQ(h.published_count(), published_before);
  EXPECT_EQ(h.texture_allocations(), allocations_before);

  // The next render of the unchanged document runs exactly the passes of the control render.
  h.reset_pass_stats();
  h.execute_plan();
  const auto stats = h.pass_stats();
  EXPECT_EQ(stats.sensor_develop_execute, control.sensor_develop_execute);
  EXPECT_EQ(stats.sensor_develop_skip, control.sensor_develop_skip);
  EXPECT_EQ(stats.geometry_execute, control.geometry_execute);
  EXPECT_EQ(stats.camera_color_execute, control.camera_color_execute);
  EXPECT_EQ(stats.mask_execute, control.mask_execute);
  EXPECT_EQ(stats.mask_skip, control.mask_skip);
  EXPECT_EQ(stats.primary_grade_execute, control.primary_grade_execute);
  EXPECT_EQ(stats.drt_execute, control.drt_execute);
  EXPECT_EQ(stats.result_content_hits, control.result_content_hits);
  EXPECT_EQ(stats.result_content_misses, control.result_content_misses);
  EXPECT_EQ(h.photo_result_ids(), results_before);
  EXPECT_EQ(h.texture_allocations(), allocations_before) << "the render reused its scratch";
}

inline void CheckAiMaskMissingAssetFailsRender(AiMaskBackendHarness& h) {
  const auto asset = MakeEllipseAsset(kAnalysisExtent, kAssetRect);
  (void)AttachAiMask(h, asset);
  h.set_reader(std::make_shared<InMemoryMaskAssetReader>());
  h.compile({});
  try {
    h.render_mask();
    FAIL() << "a render with a missing asset succeeded";
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("Mask asset " + std::string{asset.key.Value()} + " is missing"),
              std::string::npos)
        << message;
  }
  EXPECT_EQ(h.cache_entries(), 0u);
}

}  // namespace alcedo::ai_mask_test
