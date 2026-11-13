//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>

#include "decoders/dng_default_crop.hpp"
#include "edit/operators/geometry/resize_algorithm.hpp"

namespace alcedo::metal {
class MetalImage;
}

namespace alcedo::metal::utils {

void ResizeTexture(const MetalImage& src, MetalImage& dst, cv::Size dst_size,
                   ResizeDownsampleAlgorithm downsample_algorithm = ResizeDownsampleAlgorithm::Area);
void CropResizeTexture(const MetalImage& src, MetalImage& dst, const cv::Rect& crop_rect,
                       cv::Size dst_size,
                       ResizeDownsampleAlgorithm downsample_algorithm =
                           ResizeDownsampleAlgorithm::Area);
void WarpAffineLinearTexture(const MetalImage& src, MetalImage& dst, const cv::Mat& matrix,
                             cv::Size out_size, const cv::Scalar& border_value);
void WarpRectilinearTexture(const MetalImage& src, MetalImage& dst,
                            const dng::WarpRectilinear& warp);

void EncodeWarpRectilinearTexture(void* command_buffer, void* src_texture, void* dst_texture,
                                  const dng::WarpRectilinear& warp);

}  // namespace alcedo::metal::utils

#endif
