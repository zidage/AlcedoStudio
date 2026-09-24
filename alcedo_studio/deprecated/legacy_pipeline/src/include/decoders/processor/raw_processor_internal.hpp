//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <libraw/libraw.h>

#include <algorithm>
#include <cstdint>
#include <opencv2/core.hpp>
#include <optional>
#include <stdexcept>

#include "decoders/processor/raw_processor.hpp"

namespace alcedo::detail {

enum class CudaExecutionMode {
  FullFrame,
  Tiled,
};

inline constexpr int kCudaTileThresholdLongEdge = 9000;
inline constexpr int kCudaTileInnerSize         = 1024;
inline constexpr int kCudaTileHaloSize          = 16;
inline constexpr int kRcdDebayerCropRadius      = 4;

inline auto          DecodeResScaleDivisor(const DecodeRes decode_res) -> int {
  switch (decode_res) {
    case DecodeRes::FULL:
      return 1;
    case DecodeRes::HALF:
      return 2;
    case DecodeRes::QUARTER:
      return 4;
    case DecodeRes::EIGHTH:
      return 8;
    default:
      throw std::runtime_error("RawProcessor: Unknown decode resolution");
  }
}

inline auto ScaleCoordFloor(const int value, const int divisor) -> int { return value / divisor; }

inline auto ScaleCoordCeil(const int value, const int divisor) -> int {
  return (value + divisor - 1) / divisor;
}

inline auto BuildActiveAreaRect(const libraw_image_sizes_t& sizes, const cv::Size& image_size,
                                const int scale_divisor = 1) -> cv::Rect {
  const int raw_width  = std::max(static_cast<int>(sizes.raw_width), 0);
  const int raw_height = std::max(static_cast<int>(sizes.raw_height), 0);

  const int raw_left   = std::clamp(static_cast<int>(sizes.left_margin), 0, raw_width);
  const int raw_top    = std::clamp(static_cast<int>(sizes.top_margin), 0, raw_height);
  const int raw_right  = std::clamp(raw_left + static_cast<int>(sizes.width), raw_left, raw_width);
  const int raw_bottom = std::clamp(raw_top + static_cast<int>(sizes.height), raw_top, raw_height);

  const int left       = std::clamp(ScaleCoordFloor(raw_left, scale_divisor), 0, image_size.width);
  const int top        = std::clamp(ScaleCoordFloor(raw_top, scale_divisor), 0, image_size.height);
  const int right  = std::clamp(ScaleCoordCeil(raw_right, scale_divisor), left, image_size.width);
  const int bottom = std::clamp(ScaleCoordCeil(raw_bottom, scale_divisor), top, image_size.height);
  const int width  = right - left;
  const int height = bottom - top;

  if (width <= 0 || height <= 0) {
    return {0, 0, image_size.width, image_size.height};
  }
  return {left, top, width, height};
}

inline auto HasValidDefaultCrop(const libraw_image_sizes_t& sizes, const ushort default_crop[4])
    -> bool {
  const int raw_width       = std::max(static_cast<int>(sizes.raw_width), 0);
  const int raw_height      = std::max(static_cast<int>(sizes.raw_height), 0);

  const int raw_active_left = std::clamp(static_cast<int>(sizes.left_margin), 0, raw_width);
  const int raw_active_top  = std::clamp(static_cast<int>(sizes.top_margin), 0, raw_height);
  const int raw_active_right =
      std::clamp(raw_active_left + static_cast<int>(sizes.width), raw_active_left, raw_width);
  const int raw_active_bottom =
      std::clamp(raw_active_top + static_cast<int>(sizes.height), raw_active_top, raw_height);

  const int raw_crop_left   = static_cast<int>(default_crop[0]);
  const int raw_crop_top    = static_cast<int>(default_crop[1]);
  const int raw_crop_width  = static_cast<int>(default_crop[2]);
  const int raw_crop_height = static_cast<int>(default_crop[3]);
  if (raw_crop_width <= 0 || raw_crop_height <= 0) {
    return false;
  }

  const int raw_crop_right  = raw_crop_left + raw_crop_width;
  const int raw_crop_bottom = raw_crop_top + raw_crop_height;
  return raw_crop_left >= raw_active_left && raw_crop_top >= raw_active_top &&
         raw_crop_right <= raw_active_right && raw_crop_bottom <= raw_active_bottom;
}

inline auto BuildDefaultCropRect(const libraw_image_sizes_t& sizes, const ushort default_crop[4],
                                 const cv::Size& image_size, const int scale_divisor = 1)
    -> cv::Rect {
  const cv::Rect active_rect     = BuildActiveAreaRect(sizes, image_size, scale_divisor);

  // DNG DefaultCrop* uses raw-image coordinates. If the file does not provide a valid crop,
  // keep the broader active area instead.
  const int      raw_crop_left   = static_cast<int>(default_crop[0]);
  const int      raw_crop_top    = static_cast<int>(default_crop[1]);
  const int      raw_crop_width  = static_cast<int>(default_crop[2]);
  const int      raw_crop_height = static_cast<int>(default_crop[3]);
  if (!HasValidDefaultCrop(sizes, default_crop)) {
    return active_rect;
  }

  const int raw_crop_right  = raw_crop_left + raw_crop_width;
  const int raw_crop_bottom = raw_crop_top + raw_crop_height;

  const int left = std::clamp(ScaleCoordFloor(raw_crop_left, scale_divisor), 0, image_size.width);
  const int top  = std::clamp(ScaleCoordFloor(raw_crop_top, scale_divisor), 0, image_size.height);
  const int right =
      std::clamp(ScaleCoordCeil(raw_crop_right, scale_divisor), left, image_size.width);
  const int bottom =
      std::clamp(ScaleCoordCeil(raw_crop_bottom, scale_divisor), top, image_size.height);
  const int width  = right - left;
  const int height = bottom - top;

  if (width <= 0 || height <= 0) {
    return active_rect;
  }
  return {left, top, width, height};
}

inline auto BuildDecodeCropRect(const libraw_image_sizes_t& sizes, const ushort default_crop[4],
                                const cv::Size& image_size, const DecodeRes decode_res)
    -> cv::Rect {
  return BuildDefaultCropRect(sizes, default_crop, image_size, DecodeResScaleDivisor(decode_res));
}

inline auto BuildBorderLossDecodeCropRect(const libraw_image_sizes_t& sizes,
                                          const ushort default_crop[4], const cv::Size& output_size,
                                          const DecodeRes decode_res, const int source_border)
    -> cv::Rect {
  if (source_border <= 0) {
    return BuildDecodeCropRect(sizes, default_crop, output_size, decode_res);
  }

  const cv::Size source_size(output_size.width + 2 * source_border,
                             output_size.height + 2 * source_border);
  const cv::Rect source_crop = BuildDecodeCropRect(sizes, default_crop, source_size, decode_res);

  // Border-losing demosaicers (RCD and the valid-convolution Neural Engine) remove an equal
  // source border on every edge. Inset LibRaw's selected crop before mapping to output space.
  const int      left        = std::clamp(source_crop.x, 0, output_size.width);
  const int      top         = std::clamp(source_crop.y, 0, output_size.height);
  const int      right =
      std::clamp(source_crop.x + source_crop.width - 2 * source_border, left, output_size.width);
  const int bottom =
      std::clamp(source_crop.y + source_crop.height - 2 * source_border, top, output_size.height);

  if (right <= left || bottom <= top) {
    throw std::runtime_error("RawProcessor: decode crop is too small for demosaic border.");
  }
  return {left, top, right - left, bottom - top};
}

// Explicit Neural RGB ↔ CFA coordinate map. Phase crop, trailing period trim, tile
// border, and final sensor crop are separate transforms; do not collapse them into one
// `source_border` integer.
//
// Student tiled path (virtual pad): assembled RGB is same-size as aligned CFA, so
//   output_origin_in_aligned = (0, 0). Tile-local `output_border` is not subtracted again.
// Full-frame natural path: valid-convolution shrink places
//   output_origin_in_aligned = (border, border) with output_size = aligned - 2*border.
struct NeuralOutputGeometry {
  cv::Point aligned_origin_in_original;  // phase crop (sx, sy) applied once to original CFA
  cv::Point output_origin_in_aligned;    // where RGB (0,0) sits in the aligned lattice
  cv::Size  output_size;                 // assembled RGB size
};

[[nodiscard]] inline auto MakeStudentTiledNeuralOutputGeometry(const int phase_shift_x,
                                                               const int phase_shift_y,
                                                               const cv::Size& aligned_size)
    -> NeuralOutputGeometry {
  return {cv::Point(phase_shift_x, phase_shift_y), cv::Point(0, 0), aligned_size};
}

[[nodiscard]] inline auto MakeNaturalShrinkNeuralOutputGeometry(const int phase_shift_x,
                                                                const int phase_shift_y,
                                                                const cv::Size& aligned_size,
                                                                const int source_border)
    -> NeuralOutputGeometry {
  return {cv::Point(phase_shift_x, phase_shift_y), cv::Point(source_border, source_border),
          cv::Size(std::max(0, aligned_size.width - 2 * source_border),
                   std::max(0, aligned_size.height - 2 * source_border))};
}

// Map LibRaw's default/active crop from original CFA space through NeuralOutputGeometry.
inline auto BuildNeuralEngineDecodeCropRect(const libraw_image_sizes_t& sizes,
                                            const ushort default_crop[4],
                                            const cv::Size& original_cfa_size,
                                            const DecodeRes decode_res,
                                            const NeuralOutputGeometry& geometry) -> cv::Rect {
  const cv::Rect original_crop =
      BuildDecodeCropRect(sizes, default_crop, original_cfa_size, decode_res);

  // original → aligned (subtract phase once) → RGB (subtract output origin once).
  const int left = std::clamp(original_crop.x - geometry.aligned_origin_in_original.x -
                                  geometry.output_origin_in_aligned.x,
                              0, geometry.output_size.width);
  const int top = std::clamp(original_crop.y - geometry.aligned_origin_in_original.y -
                                 geometry.output_origin_in_aligned.y,
                             0, geometry.output_size.height);
  const int right =
      std::clamp(original_crop.x + original_crop.width - geometry.aligned_origin_in_original.x -
                     geometry.output_origin_in_aligned.x,
                 left, geometry.output_size.width);
  const int bottom =
      std::clamp(original_crop.y + original_crop.height - geometry.aligned_origin_in_original.y -
                     geometry.output_origin_in_aligned.y,
                 top, geometry.output_size.height);

  if (right <= left || bottom <= top) {
    throw std::runtime_error("RawProcessor: Neural Engine decode crop is empty after phase-align.");
  }
  return {left, top, right - left, bottom - top};
}

// Adapter for callers that still pass a single equal border + precomputed RGB size
// (full-frame natural path, tests). Prefer NeuralOutputGeometry for new code.
inline auto BuildNeuralEngineDecodeCropRect(const libraw_image_sizes_t& sizes,
                                            const ushort default_crop[4],
                                            const cv::Size& original_cfa_size,
                                            const cv::Size& rgb_output_size,
                                            const DecodeRes decode_res, const int source_border,
                                            const int phase_shift_x, const int phase_shift_y)
    -> cv::Rect {
  NeuralOutputGeometry geometry;
  geometry.aligned_origin_in_original = {phase_shift_x, phase_shift_y};
  geometry.output_origin_in_aligned   = {source_border, source_border};
  geometry.output_size                = rgb_output_size;
  return BuildNeuralEngineDecodeCropRect(sizes, default_crop, original_cfa_size, decode_res,
                                         geometry);
}

inline auto BuildRcdDecodeCropRect(const libraw_image_sizes_t& sizes, const ushort default_crop[4],
                                   const cv::Size& rcd_output_size, const DecodeRes decode_res,
                                   const int rcd_radius = kRcdDebayerCropRadius) -> cv::Rect {
  return BuildBorderLossDecodeCropRect(sizes, default_crop, rcd_output_size, decode_res,
                                       rcd_radius);
}

inline auto ResolveRawDemosaicMethod(const RawParams& params, const RawCfaKind cfa_kind)
    -> RawDemosaicMethod {
  // Thumbnail/preview decodes must remain on the low-cost legacy path even when the user
  // explicitly selected Neural Engine for full-resolution editing.
  if (params.decode_res_ != DecodeRes::FULL) {
    return RawDemosaicMethod::Legacy;
  }
  if (params.demosaic_method_ != RawDemosaicMethod::Default) {
    return params.demosaic_method_;
  }
  return cfa_kind == RawCfaKind::XTrans6x6 ? RawDemosaicMethod::NeuralEngine
                                           : RawDemosaicMethod::Legacy;
}

inline auto IsFullImageRect(const cv::Rect& rect, const cv::Size& image_size) -> bool {
  return rect.x == 0 && rect.y == 0 && rect.width == image_size.width &&
         rect.height == image_size.height;
}

auto SelectCudaExecutionMode(const RawParams& params, const RawCfaPattern& cfa_pattern,
                             const cv::Rect& active_rect) -> CudaExecutionMode;

void SetCudaExecutionModeOverrideForTesting(const std::optional<CudaExecutionMode>& mode);
auto GetCudaExecutionModeOverrideForTesting() -> std::optional<CudaExecutionMode>;

}  // namespace alcedo::detail
