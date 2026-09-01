//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include "../graph/grade_owned_mask_support.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/active_raster_mask.hpp"
#include "edit/mask/mask_store.hpp"
#include "edit/pipeline/pipeline_apply_request.hpp"
#include "edit/runtime/renderer.hpp"
#include "image/image_buffer.hpp"

namespace alcedo::multi_mask_qualification {

inline constexpr float kSettledMatchTol = 2.0e-5f;
inline constexpr float kPreviewDiffMin  = 1.0e-3f;
inline constexpr std::uint32_t kProductMaskEdge = 32;

inline auto HostIsFinite(const std::shared_ptr<ImageBuffer>& image) -> bool {
  if (!image || !image->cpu_data_valid_) {
    return false;
  }
  const auto& mat = image->GetCPUData();
  if (mat.empty() || mat.type() != CV_32FC4) {
    return false;
  }
  for (int row = 0; row < mat.rows; ++row) {
    const auto* pixels = mat.ptr<cv::Vec4f>(row);
    for (int col = 0; col < mat.cols; ++col) {
      for (int channel = 0; channel < 4; ++channel) {
        if (!std::isfinite(pixels[col][channel])) {
          return false;
        }
      }
    }
  }
  return true;
}

inline auto HostMaxAbsError(ImageBuffer& lhs, ImageBuffer& rhs) -> float {
  const auto& a = lhs.GetCPUData();
  const auto& b = rhs.GetCPUData();
  if (a.size() != b.size() || a.type() != b.type()) {
    return INFINITY;
  }
  return static_cast<float>(cv::norm(a, b, cv::NORM_INF));
}

/**
 * @brief Editor preview may carry active Brush pixels; thumbnail/export must not.
 *
 * Settled bypass pixels match the saved asset, differ from the active override, and
 * leave the persistent MaskStore file unchanged. One-shot resources are released.
 */
template <class Renderer>
void BackgroundRenderUsesSettledAssetsOnly(Renderer& renderer, PipelineDocument& document,
                                           const std::shared_ptr<ImageBuffer>& image) {
  MaskAsset settled;
  settled.descriptor.extent = {kProductMaskEdge, kProductMaskEdge};
  settled.pixels.assign(static_cast<std::size_t>(kProductMaskEdge) * kProductMaskEdge, 255);
  settled.key = renderer.MaskAssets().Put(settled.descriptor, settled.pixels);
  grade_mask_test::AddBrushMask(document, MaskId{"mask.raster"}, settled.key, settled.descriptor);

  PipelineApplyRequest settled_session;
  settled_session.decode_res          = DecodeRes::FULL;
  settled_session.require_host_output = true;
  settled_session.cache_policy        = RenderCachePolicy::UseSessionCache;
  const auto settled_frame            = renderer.Render(image, settled_session);
  ASSERT_TRUE(HostIsFinite(settled_frame));
  const auto session_published = renderer.SessionResources().published_result_count;
  EXPECT_GT(session_published, 0U);

  ActiveRasterMaskInput active;
  active.owner_node_id      = document.PrimaryGrade()->Id();
  active.mask_id            = MaskId{"mask.raster"};
  active.session_generation = 1;
  active.content_revision   = 1;
  active.descriptor         = settled.descriptor;
  active.pixels = std::make_shared<const std::vector<std::uint8_t>>(
      static_cast<std::size_t>(kProductMaskEdge) * kProductMaskEdge, std::uint8_t{0});
  active.dirty_rectangle = {0, 0, static_cast<std::int32_t>(kProductMaskEdge),
                            static_cast<std::int32_t>(kProductMaskEdge)};

  PipelineApplyRequest preview;
  preview.decode_res          = DecodeRes::FULL;
  preview.require_host_output = true;
  preview.cache_policy        = RenderCachePolicy::UseSessionCache;
  preview.active_raster_masks.push_back(active);
  const auto preview_frame = renderer.Render(image, preview);
  ASSERT_TRUE(HostIsFinite(preview_frame));
  EXPECT_GT(HostMaxAbsError(*settled_frame, *preview_frame), kPreviewDiffMin);
  const auto published_after_preview = renderer.SessionResources().published_result_count;
  EXPECT_GT(published_after_preview, 0U);

  PipelineApplyRequest background;
  background.decode_res          = DecodeRes::FULL;
  background.require_host_output = true;
  background.cache_policy        = RenderCachePolicy::BypassSessionCache;
  EXPECT_TRUE(background.active_raster_masks.empty());
  EXPECT_FALSE(background.allow_active_raster_preview);
  const auto background_frame = renderer.Render(image, background);
  ASSERT_TRUE(HostIsFinite(background_frame));
  EXPECT_LT(HostMaxAbsError(*settled_frame, *background_frame), kSettledMatchTol);
  EXPECT_GT(HostMaxAbsError(*preview_frame, *background_frame), kPreviewDiffMin);
  EXPECT_EQ(renderer.OneShotPublishedResultCount(), 0U);
  EXPECT_EQ(renderer.OneShotResources().texture_pool_used_bytes, 0U);
  EXPECT_EQ(renderer.SessionResources().published_result_count, published_after_preview);

  const auto loaded = renderer.MaskAssets().Load(settled.key);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->pixels, settled.pixels);
  EXPECT_EQ(loaded->key, settled.key);
}

}  // namespace alcedo::multi_mask_qualification
