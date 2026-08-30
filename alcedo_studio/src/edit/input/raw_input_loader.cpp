//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/input/raw_input_loader.hpp"

#include <libraw/libraw.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <opencv2/core.hpp>
#include <stdexcept>
#include <utility>

#include "decoders/libraw_unpack_guard.hpp"
#include "decoders/processor/raw_normalization.hpp"
#include "decoders/processor/raw_rgb_normalization.hpp"

namespace alcedo {
namespace {

constexpr int kRcdOutputCropRadius = 4;

auto          MixU64(std::uint64_t hash, std::uint64_t value) -> std::uint64_t {
  hash ^= value;
  hash *= 1099511628211ull;
  return hash;
}

auto HashCfaPattern(const RawCfaPattern& pattern) -> std::uint64_t {
  std::uint64_t hash = MixU64(14695981039346656037ull, static_cast<std::uint64_t>(pattern.kind));
  if (pattern.kind == RawCfaKind::XTrans6x6) {
    for (int i = 0; i < 36; ++i) {
      hash = MixU64(hash, static_cast<std::uint64_t>(pattern.xtrans_pattern.raw_fc[i] + 1));
    }
  } else {
    for (int i = 0; i < 4; ++i) {
      hash = MixU64(hash, static_cast<std::uint64_t>(pattern.bayer_pattern.raw_fc[i] + 1));
    }
  }
  return hash;
}

auto HashDngWarp(const std::optional<dng::WarpRectilinear>& warp) -> std::uint64_t {
  if (!warp.has_value()) return 0;
  std::uint64_t hash = MixU64(14695981039346656037ull, warp->coefficient_set_count);
  for (const auto& set : warp->coefficient_sets) {
    for (double value : set) {
      std::uint64_t bits = 0;
      static_assert(sizeof(bits) == sizeof(value));
      std::memcpy(&bits, &value, sizeof(bits));
      hash = MixU64(hash, bits);
    }
  }
  std::uint64_t center_x = 0;
  std::uint64_t center_y = 0;
  std::memcpy(&center_x, &warp->center_x, sizeof(center_x));
  std::memcpy(&center_y, &warp->center_y, sizeof(center_y));
  return MixU64(MixU64(hash, center_x), center_y);
}

auto FillSourceKey(PreparedRawInput& input, std::uint64_t encoded_hash,
                   std::uint64_t encoded_byte_count) -> void {
  input.content_key.content_hash        = encoded_hash;
  input.source_key.encoded_content_hash = encoded_hash;
  input.source_key.encoded_byte_count   = encoded_byte_count;
  input.source_key.input_kind           = input.input_kind;
  input.source_key.cfa_hash             = HashCfaPattern(input.cfa_pattern);
  input.source_key.dng_warp_hash        = HashDngWarp(input.dng_warp_rectilinear);
  input.source_key.downsample_passes    = input.downsample_passes;
  input.source_key.sensor_active_area   = input.sensor_active_area;
  input.source_key.orientation_flip     = input.sensor.orientation_flip;
  input.source_key.preparation_version  = kRawInputPreparationVersion;
}

auto ScaleCoordFloor(int value, int divisor) -> int { return value / divisor; }

auto ScaleCoordCeil(int value, int divisor) -> int { return (value + divisor - 1) / divisor; }

auto ScaleDivisor(std::uint8_t downsample_passes) -> int {
  return 1 << static_cast<int>(downsample_passes);
}

auto BuildActiveAreaRect(const RawSensorGeometry& sensor, Extent2D image_size, int scale_divisor)
    -> RectI {
  const int raw_width  = std::max(sensor.raw_width, 0);
  const int raw_height = std::max(sensor.raw_height, 0);
  const int raw_left   = std::clamp(sensor.left_margin, 0, raw_width);
  const int raw_top    = std::clamp(sensor.top_margin, 0, raw_height);
  const int raw_right  = std::clamp(raw_left + sensor.width, raw_left, raw_width);
  const int raw_bottom = std::clamp(raw_top + sensor.height, raw_top, raw_height);

  const int img_w      = static_cast<int>(image_size.width);
  const int img_h      = static_cast<int>(image_size.height);
  const int left       = std::clamp(ScaleCoordFloor(raw_left, scale_divisor), 0, img_w);
  const int top        = std::clamp(ScaleCoordFloor(raw_top, scale_divisor), 0, img_h);
  const int right      = std::clamp(ScaleCoordCeil(raw_right, scale_divisor), left, img_w);
  const int bottom     = std::clamp(ScaleCoordCeil(raw_bottom, scale_divisor), top, img_h);
  if (right - left <= 0 || bottom - top <= 0) {
    return RectI{0, 0, img_w, img_h};
  }
  return RectI{left, top, right - left, bottom - top};
}

auto HasValidDefaultCrop(const RawSensorGeometry& sensor) -> bool {
  const int raw_width  = std::max(sensor.raw_width, 0);
  const int raw_height = std::max(sensor.raw_height, 0);
  const int raw_left   = std::clamp(sensor.left_margin, 0, raw_width);
  const int raw_top    = std::clamp(sensor.top_margin, 0, raw_height);
  const int raw_right  = std::clamp(raw_left + sensor.width, raw_left, raw_width);
  const int raw_bottom = std::clamp(raw_top + sensor.height, raw_top, raw_height);
  const int crop_w     = static_cast<int>(sensor.default_crop[2]);
  const int crop_h     = static_cast<int>(sensor.default_crop[3]);
  if (crop_w <= 0 || crop_h <= 0) {
    return false;
  }
  const int crop_l = static_cast<int>(sensor.default_crop[0]);
  const int crop_t = static_cast<int>(sensor.default_crop[1]);
  return crop_l >= raw_left && crop_t >= raw_top && crop_l + crop_w <= raw_right &&
         crop_t + crop_h <= raw_bottom;
}

auto BuildDecodeCropRect(const RawSensorGeometry& sensor, Extent2D image_size, int scale_divisor)
    -> RectI {
  const RectI active = BuildActiveAreaRect(sensor, image_size, scale_divisor);
  if (!HasValidDefaultCrop(sensor)) {
    return active;
  }
  const int img_w = static_cast<int>(image_size.width);
  const int img_h = static_cast<int>(image_size.height);
  const int left  = std::clamp(
      ScaleCoordFloor(static_cast<int>(sensor.default_crop[0]), scale_divisor), 0, img_w);
  const int top = std::clamp(
      ScaleCoordFloor(static_cast<int>(sensor.default_crop[1]), scale_divisor), 0, img_h);
  const int right =
      std::clamp(ScaleCoordCeil(static_cast<int>(sensor.default_crop[0] + sensor.default_crop[2]),
                                scale_divisor),
                 left, img_w);
  const int bottom =
      std::clamp(ScaleCoordCeil(static_cast<int>(sensor.default_crop[1] + sensor.default_crop[3]),
                                scale_divisor),
                 top, img_h);
  if (right - left <= 0 || bottom - top <= 0) {
    return active;
  }
  return RectI{left, top, right - left, bottom - top};
}

auto BuildBorderLossCrop(const RawSensorGeometry& sensor, Extent2D output_size, int scale_divisor,
                         int source_border) -> RectI {
  if (source_border <= 0) {
    return BuildDecodeCropRect(sensor, output_size, scale_divisor);
  }
  const Extent2D source_size{output_size.width + static_cast<std::uint32_t>(2 * source_border),
                             output_size.height + static_cast<std::uint32_t>(2 * source_border)};
  const RectI    source_crop = BuildDecodeCropRect(sensor, source_size, scale_divisor);
  const int      out_w       = static_cast<int>(output_size.width);
  const int      out_h       = static_cast<int>(output_size.height);
  const int      left        = std::clamp(source_crop.x, 0, out_w);
  const int      top         = std::clamp(source_crop.y, 0, out_h);
  const int right  = std::clamp(source_crop.x + source_crop.width - 2 * source_border, left, out_w);
  const int bottom = std::clamp(source_crop.y + source_crop.height - 2 * source_border, top, out_h);
  if (right <= left || bottom <= top) {
    throw std::runtime_error("RawInputLoader: decode crop is too small for demosaic border");
  }
  return RectI{left, top, right - left, bottom - top};
}

auto OrientedExtent(Extent2D extent, int flip) -> Extent2D {
  if (flip == 5 || flip == 6) {
    return Extent2D{extent.height, extent.width};
  }
  return extent;
}

void FillOutputGeometry(PreparedRawInput& input, DecodeRes decode_res_for_full_reference) {
  (void)decode_res_for_full_reference;
  const std::uint8_t passes  = input.downsample_passes;
  const int          divisor = ScaleDivisor(passes);
  const Extent2D     host    = input.host_extent;

  if (input.input_kind == RawInputKind::DebayeredRgb) {
    const RectI crop            = BuildDecodeCropRect(input.sensor, host, divisor);
    input.demosaic_output_crop  = crop;
    input.develop_output_extent = OrientedExtent(
        Extent2D{static_cast<std::uint32_t>(crop.width), static_cast<std::uint32_t>(crop.height)},
        input.sensor.orientation_flip);
    const int      full_div = 1;
    const Extent2D full_host{static_cast<std::uint32_t>(std::max(input.sensor.raw_width, 0)),
                             static_cast<std::uint32_t>(std::max(input.sensor.raw_height, 0))};
    const RectI    full_crop = BuildDecodeCropRect(input.sensor, full_host, full_div);
    input.full_reference_extent =
        OrientedExtent(Extent2D{static_cast<std::uint32_t>(full_crop.width),
                                static_cast<std::uint32_t>(full_crop.height)},
                       input.sensor.orientation_flip);
    input.sensor_active_area = BuildActiveAreaRect(input.sensor, host, divisor);
    return;
  }

  const bool     bayer  = input.cfa_pattern.kind == RawCfaKind::Bayer2x2;
  const int      border = bayer ? kRcdOutputCropRadius : 0;
  const Extent2D demosaic_extent{
      static_cast<std::uint32_t>(std::max(0, static_cast<int>(host.width) - 2 * border)),
      static_cast<std::uint32_t>(std::max(0, static_cast<int>(host.height) - 2 * border))};
  const RectI crop = bayer ? BuildBorderLossCrop(input.sensor, demosaic_extent, divisor, border)
                           : BuildDecodeCropRect(input.sensor, host, divisor);
  input.demosaic_output_crop  = crop;
  input.develop_output_extent = OrientedExtent(
      Extent2D{static_cast<std::uint32_t>(crop.width), static_cast<std::uint32_t>(crop.height)},
      input.sensor.orientation_flip);

  const Extent2D full_host{static_cast<std::uint32_t>(std::max(input.sensor.raw_width, 0)),
                           static_cast<std::uint32_t>(std::max(input.sensor.raw_height, 0))};
  const Extent2D full_demosaic{
      static_cast<std::uint32_t>(std::max(0, static_cast<int>(full_host.width) - 2 * border)),
      static_cast<std::uint32_t>(std::max(0, static_cast<int>(full_host.height) - 2 * border))};
  const RectI full_crop = bayer ? BuildBorderLossCrop(input.sensor, full_demosaic, 1, border)
                                : BuildDecodeCropRect(input.sensor, full_host, 1);
  input.full_reference_extent =
      OrientedExtent(Extent2D{static_cast<std::uint32_t>(full_crop.width),
                              static_cast<std::uint32_t>(full_crop.height)},
                     input.sensor.orientation_flip);
  input.sensor_active_area = BuildActiveAreaRect(input.sensor, host, divisor);
}

auto CopyPlane(const cv::Mat& src, HostPixelFormat format) -> HostImagePlane {
  HostImagePlane plane;
  plane.extent =
      Extent2D{static_cast<std::uint32_t>(src.cols), static_cast<std::uint32_t>(src.rows)};
  plane.stride_bytes      = static_cast<std::uint32_t>(src.cols * src.elemSize());
  plane.format            = format;
  const std::size_t bytes = plane.ByteCount();
  auto              storage =
      std::shared_ptr<std::byte>(new std::byte[bytes], std::default_delete<std::byte[]>());
  if (src.isContinuous() && static_cast<std::size_t>(src.step) == plane.stride_bytes) {
    std::memcpy(storage.get(), src.data, bytes);
  } else {
    for (int y = 0; y < src.rows; ++y) {
      std::memcpy(storage.get() + static_cast<std::size_t>(y) * plane.stride_bytes, src.ptr(y),
                  plane.stride_bytes);
    }
  }
  plane.bytes = std::const_pointer_cast<const std::byte>(storage);
  return plane;
}

auto WrapU16Cfa(const HostImagePlane& plane) -> cv::Mat {
  if (plane.format != HostPixelFormat::U16Cfa || !plane.bytes) {
    throw std::runtime_error("RawInputLoader: expected U16 CFA plane");
  }
  return cv::Mat(static_cast<int>(plane.extent.height), static_cast<int>(plane.extent.width),
                 CV_16UC1, const_cast<std::byte*>(plane.bytes.get()), plane.stride_bytes);
}

auto WrapF32Rgba(const HostImagePlane& plane) -> cv::Mat {
  if (plane.format != HostPixelFormat::F32Rgba || !plane.bytes) {
    throw std::runtime_error("RawInputLoader: expected F32 RGBA plane");
  }
  return cv::Mat(static_cast<int>(plane.extent.height), static_cast<int>(plane.extent.width),
                 CV_32FC4, const_cast<std::byte*>(plane.bytes.get()), plane.stride_bytes);
}

auto Rgb32fToRgbaPlane(const cv::Mat& rgb) -> HostImagePlane {
  if (rgb.type() != CV_32FC3 || rgb.empty()) {
    throw std::runtime_error("RawInputLoader: expected F32 RGB mat");
  }
  HostImagePlane plane;
  plane.extent =
      Extent2D{static_cast<std::uint32_t>(rgb.cols), static_cast<std::uint32_t>(rgb.rows)};
  plane.stride_bytes      = plane.extent.width * 4U * static_cast<std::uint32_t>(sizeof(float));
  plane.format            = HostPixelFormat::F32Rgba;
  const std::size_t bytes = plane.ByteCount();
  auto              storage =
      std::shared_ptr<std::byte>(new std::byte[bytes], std::default_delete<std::byte[]>());
  auto* out = reinterpret_cast<float*>(storage.get());
  for (int y = 0; y < rgb.rows; ++y) {
    const auto* row = rgb.ptr<float>(y);
    for (int x = 0; x < rgb.cols; ++x) {
      const auto pixel = static_cast<std::size_t>(y) * plane.extent.width + static_cast<std::size_t>(x);
      out[pixel * 4 + 0] = row[static_cast<std::size_t>(x) * 3 + 0];
      out[pixel * 4 + 1] = row[static_cast<std::size_t>(x) * 3 + 1];
      out[pixel * 4 + 2] = row[static_cast<std::size_t>(x) * 3 + 2];
      out[pixel * 4 + 3] = 1.0f;
    }
  }
  plane.bytes = std::const_pointer_cast<const std::byte>(storage);
  return plane;
}

auto ExtractRgb32f(const cv::Mat& src) -> cv::Mat {
  if (src.type() == CV_32FC3) {
    return src;
  }
  if (src.type() == CV_32FC4) {
    cv::Mat rgb(src.rows, src.cols, CV_32FC3);
    const int from_to[] = {0, 0, 1, 1, 2, 2};
    cv::mixChannels(&src, 1, &rgb, 1, from_to, 3);
    return rgb;
  }
  throw std::runtime_error("RawInputLoader: expected 3 or 4 channel F32 RGB");
}

auto CropMatToDecodeArea(const cv::Mat& src, const RawSensorGeometry& sensor) -> cv::Mat {
  const Extent2D host{static_cast<std::uint32_t>(src.cols), static_cast<std::uint32_t>(src.rows)};
  const RectI    crop = BuildDecodeCropRect(sensor, host, 1);
  const cv::Rect rect(crop.x, crop.y, crop.width, crop.height);
  if (rect.x == 0 && rect.y == 0 && rect.width == src.cols && rect.height == src.rows) {
    return src.clone();
  }
  return src(rect).clone();
}

auto CopyDebayeredRgbPlane(LibRaw& raw, const RawSensorGeometry& sensor) -> HostImagePlane {
  const auto& raw_data   = raw.imgdata.rawdata;
  const auto& idata      = raw.imgdata.idata;
  const auto& sizes      = raw.imgdata.sizes;
  const int   raw_width  = static_cast<int>(sizes.raw_width);
  const int   raw_height = static_cast<int>(sizes.raw_height);

  cv::Mat rgb32f;
  if (raw_data.color3_image != nullptr) {
    const std::size_t row_step = sizes.raw_pitch != 0
                                     ? static_cast<std::size_t>(sizes.raw_pitch)
                                     : static_cast<std::size_t>(raw_width) * sizeof(std::uint16_t) * 3;
    cv::Mat           view(raw_height, raw_width, CV_16UC3, raw_data.color3_image, row_step);
    rgb32f = raw_norm::ConvertUnpackedRgbToFloat(CropMatToDecodeArea(view, sensor), raw_data.color);
  } else if (raw_data.float3_image != nullptr) {
    const std::size_t row_step = sizes.raw_pitch != 0
                                     ? static_cast<std::size_t>(sizes.raw_pitch)
                                     : static_cast<std::size_t>(raw_width) * sizeof(float) * 3;
    cv::Mat           view(raw_height, raw_width, CV_32FC3, raw_data.float3_image, row_step);
    rgb32f = CropMatToDecodeArea(view, sensor);
  } else if (raw_data.color4_image != nullptr && idata.colors == 3) {
    const std::size_t row_step = sizes.raw_pitch != 0
                                     ? static_cast<std::size_t>(sizes.raw_pitch)
                                     : static_cast<std::size_t>(raw_width) * sizeof(std::uint16_t) * 4;
    cv::Mat           view(raw_height, raw_width, CV_16UC4, raw_data.color4_image, row_step);
    rgb32f = ExtractRgb32f(
        raw_norm::ConvertUnpackedRgbToFloat(CropMatToDecodeArea(view, sensor), raw_data.color));
  } else if (raw_data.float4_image != nullptr && idata.colors == 3) {
    const std::size_t row_step = sizes.raw_pitch != 0
                                     ? static_cast<std::size_t>(sizes.raw_pitch)
                                     : static_cast<std::size_t>(raw_width) * sizeof(float) * 4;
    cv::Mat           view(raw_height, raw_width, CV_32FC4, raw_data.float4_image, row_step);
    rgb32f = ExtractRgb32f(CropMatToDecodeArea(view, sensor));
  } else {
    throw std::runtime_error("RawInputLoader: direct RGB input is missing a 3-channel source");
  }
  return Rgb32fToRgbaPlane(rgb32f);
}

void CollapseSensorToHost(RawSensorGeometry& sensor, Extent2D host) {
  const auto width  = static_cast<std::int32_t>(host.width);
  const auto height = static_cast<std::int32_t>(host.height);
  sensor.raw_width    = width;
  sensor.raw_height   = height;
  sensor.width        = width;
  sensor.height       = height;
  sensor.left_margin  = 0;
  sensor.top_margin   = 0;
  sensor.default_crop[0] = 0;
  sensor.default_crop[1] = 0;
  sensor.default_crop[2] = static_cast<std::uint16_t>(std::min(width, 65535));
  sensor.default_crop[3] = static_cast<std::uint16_t>(std::min(height, 65535));
}

auto DownsampleRgba2x(const cv::Mat& src) -> cv::Mat {
  const int out_rows = src.rows / 2;
  const int out_cols = src.cols / 2;
  if (out_rows < 1 || out_cols < 1) {
    throw std::runtime_error("RawInputLoader: RGB too small to downsample");
  }
  cv::Mat dst(out_rows, out_cols, CV_32FC4);
  for (int y = 0; y < out_rows; ++y) {
    const auto* row0 = src.ptr<cv::Vec4f>(2 * y);
    const auto* row1 = src.ptr<cv::Vec4f>(2 * y + 1);
    auto*       dest = dst.ptr<cv::Vec4f>(y);
    for (int x = 0; x < out_cols; ++x) {
      dest[x] = (row0[2 * x] + row0[2 * x + 1] + row1[2 * x] + row1[2 * x + 1]) * 0.25f;
    }
  }
  return dst;
}

void DownsampleInPlace(PreparedRawInput& input, std::uint8_t passes) {
  if (passes == 0) {
    return;
  }
  if (input.input_kind == RawInputKind::DebayeredRgb) {
    cv::Mat current = WrapF32Rgba(input.pixels).clone();
    for (std::uint8_t i = 0; i < passes; ++i) {
      current = DownsampleRgba2x(current);
    }
    input.pixels            = CopyPlane(current, HostPixelFormat::F32Rgba);
    input.host_extent       = input.pixels.extent;
    input.downsample_passes = passes;
    return;
  }
  cv::Mat current = WrapU16Cfa(input.pixels).clone();
  for (std::uint8_t i = 0; i < passes; ++i) {
    if (current.cols < 2 || current.rows < 2) {
      throw std::runtime_error("RawInputLoader: CFA too small to downsample");
    }
    current = DownsampleRaw2x(current, input.cfa_pattern);
  }
  input.pixels            = CopyPlane(current, HostPixelFormat::U16Cfa);
  input.host_extent       = input.pixels.extent;
  input.downsample_passes = passes;
}

auto LinearizationFromLibRaw(LibRaw& raw) -> RawLinearizationParams {
  RawLinearizationParams params;
  const auto             curve = raw_norm::BuildLinearizationCurve(raw.imgdata.rawdata);
  for (int c = 0; c < 4; ++c) {
    params.black_level[c] = curve.black_level[c];
    params.white_level[c] = curve.white_level[c];
    params.cam_mul[c]     = raw.imgdata.color.cam_mul[c];
  }
  params.apply_as_shot_wb =
      (raw.imgdata.color.as_shot_wb_applied & LIBRAW_ASWB_APPLIED) == 0 ? 1 : 0;
  const int tile_width    = raw.imgdata.rawdata.color.cblack[4];
  const int tile_height   = raw.imgdata.rawdata.color.cblack[5];
  const int entries       = tile_width * tile_height;
  if (entries > 0 && entries <= 36) {
    params.black_tile_width  = tile_width;
    params.black_tile_height = tile_height;
    for (int i = 0; i < entries; ++i) {
      params.pattern_black[i] = static_cast<float>(raw.imgdata.rawdata.color.cblack[6 + i]);
    }
  }
  return params;
}

auto SensorFromLibRaw(const LibRaw& raw) -> RawSensorGeometry {
  RawSensorGeometry sensor;
  const auto&       sizes = raw.imgdata.sizes;
  sensor.raw_width        = sizes.raw_width;
  sensor.raw_height       = sizes.raw_height;
  sensor.width            = sizes.width;
  sensor.height           = sizes.height;
  sensor.left_margin      = sizes.left_margin;
  sensor.top_margin       = sizes.top_margin;
  sensor.orientation_flip = sizes.flip;
  const auto& crop        = raw.imgdata.color.dng_levels.default_crop;
  for (int i = 0; i < 4; ++i) {
    sensor.default_crop[i] = crop[i];
  }
  return sensor;
}

void FillColorContext(LibRaw& raw, RawRuntimeColorContext& ctx) {
  ctx.valid_                  = true;
  ctx.output_in_camera_space_ = true;
  for (int i = 0; i < 3; ++i) {
    ctx.cam_mul_[i] = raw.imgdata.color.cam_mul[i];
    ctx.pre_mul_[i] = raw.imgdata.color.pre_mul[i];
  }
  ctx.camera_make_  = raw.imgdata.idata.make;
  ctx.camera_model_ = raw.imgdata.idata.model;
}

auto FinishPrepared(PreparedRawInput input, DecodeRes decode_res, std::uint64_t encoded_hash,
                    std::uint64_t encoded_byte_count) -> PreparedRawInput {
  const auto passes = DecodeResToDownsamplePasses(decode_res);
  DownsampleInPlace(input, passes);
  FillOutputGeometry(input, DecodeRes::FULL);
  FillSourceKey(input, encoded_hash, encoded_byte_count);
  input.working_space = SceneWorkingSpace::CameraRgb;
  return input;
}

void ThrowIfUnsupported(LibRaw& raw, RawInputKind kind, const RawCfaPattern& pattern) {
  if (raw.imgdata.idata.is_foveon != 0U || raw.is_fuji_rotated() != 0) {
    throw std::runtime_error("RawInputLoader: unsupported CFA layout");
  }
  if (kind == RawInputKind::Unsupported) {
    throw std::runtime_error("RawInputLoader: unsupported raw input");
  }
  if (kind == RawInputKind::BayerRaw) {
    if (raw.imgdata.idata.filters == 0U || raw.imgdata.idata.filters == 1U) {
      throw std::runtime_error("RawInputLoader: unsupported CFA layout");
    }
    if (pattern.kind == RawCfaKind::Bayer2x2 && !IsClassic2x2Bayer(pattern.bayer_pattern)) {
      throw std::runtime_error("RawInputLoader: unsupported 2x2 CFA pattern");
    }
  }
}

}  // namespace

auto PreparedRawInput::CompileSource() const -> DevelopCompileSource {
  DevelopCompileSource source;
  if (input_kind == RawInputKind::DebayeredRgb) {
    source.kind = DevelopInputKind::DirectRgb;
  } else if (cfa_pattern.kind == RawCfaKind::XTrans6x6) {
    source.kind = DevelopInputKind::XTransCfa;
  } else {
    source.kind = DevelopInputKind::BayerCfa;
  }
  source.host_extent           = host_extent;
  source.develop_output_extent = develop_output_extent;
  source.full_reference_extent = full_reference_extent;
  source.sensor_active_area    = sensor_active_area;
  source.downsample_passes     = downsample_passes;
  return source;
}

auto RawInputLoader::FromUnpackedCfa(HostImagePlane plane, RawCfaPattern pattern,
                                     RawLinearizationParams linearization, RawSensorGeometry sensor,
                                     DecodeRes decode_res) -> PreparedRawInput {
  if (plane.format != HostPixelFormat::U16Cfa || plane.extent.Empty() || !plane.bytes) {
    throw std::runtime_error("RawInputLoader::FromUnpackedCfa: U16 CFA plane required");
  }
  if (pattern.kind != RawCfaKind::Bayer2x2 && pattern.kind != RawCfaKind::XTrans6x6) {
    throw std::runtime_error("RawInputLoader::FromUnpackedCfa: unsupported CFA");
  }
  if (pattern.kind == RawCfaKind::Bayer2x2 && !IsClassic2x2Bayer(pattern.bayer_pattern)) {
    throw std::runtime_error("RawInputLoader::FromUnpackedCfa: unsupported 2x2 CFA pattern");
  }
  if (sensor.raw_width == 0) {
    sensor.raw_width  = static_cast<std::int32_t>(plane.extent.width);
    sensor.raw_height = static_cast<std::int32_t>(plane.extent.height);
    sensor.width      = sensor.raw_width;
    sensor.height     = sensor.raw_height;
  }

  PreparedRawInput input;
  input.pixels                                = std::move(plane);
  input.host_extent                           = input.pixels.extent;
  input.input_kind                            = RawInputKind::BayerRaw;
  input.cfa_pattern                           = pattern;
  input.linearization                         = linearization;
  input.sensor                                = sensor;
  input.color_context.valid_                  = true;
  input.color_context.output_in_camera_space_ = true;
  for (int i = 0; i < 3; ++i) {
    input.color_context.cam_mul_[i] = linearization.cam_mul[i];
  }
  const auto encoded_hash  = HashContentBytes(input.pixels.Span());
  const auto encoded_bytes = input.pixels.ByteCount();
  return FinishPrepared(std::move(input), decode_res, encoded_hash, encoded_bytes);
}

auto RawInputLoader::FromDirectRgb(HostImagePlane plane, RawSensorGeometry sensor)
    -> PreparedRawInput {
  if (plane.format != HostPixelFormat::F32Rgba || plane.extent.Empty() || !plane.bytes) {
    throw std::runtime_error("RawInputLoader::FromDirectRgb: F32 RGBA plane required");
  }
  if (sensor.raw_width == 0) {
    sensor.raw_width  = static_cast<std::int32_t>(plane.extent.width);
    sensor.raw_height = static_cast<std::int32_t>(plane.extent.height);
    sensor.width      = sensor.raw_width;
    sensor.height     = sensor.raw_height;
  }
  PreparedRawInput input;
  input.pixels                                = std::move(plane);
  input.host_extent                           = input.pixels.extent;
  input.input_kind                            = RawInputKind::DebayeredRgb;
  input.sensor                                = sensor;
  input.linearization.apply_as_shot_wb        = 0;
  input.color_context.valid_                  = true;
  input.color_context.output_in_camera_space_ = true;
  const auto encoded_hash                     = HashContentBytes(input.pixels.Span());
  const auto encoded_bytes                    = input.pixels.ByteCount();
  return FinishPrepared(std::move(input), DecodeRes::FULL, encoded_hash, encoded_bytes);
}

auto RawInputLoader::LoadEncoded(std::span<const std::byte> encoded, DecodeRes decode_res)
    -> PreparedRawInput {
  if (encoded.empty()) {
    throw std::runtime_error("RawInputLoader::LoadEncoded: empty buffer");
  }
  const auto dng_metadata = dng::ExtractMetadata(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size()));
  // LibRaw is larger than the macOS product worker stack; it must not be a local object.
  auto       raw          = std::make_unique<LibRaw>();
  if (raw->open_buffer(const_cast<void*>(static_cast<const void*>(encoded.data())),
                       encoded.size()) != LIBRAW_SUCCESS) {
    throw std::runtime_error("RawInputLoader::LoadEncoded: LibRaw open_buffer failed");
  }
  if (libraw_guard::Unpack(*raw) != LIBRAW_SUCCESS) {
    raw->recycle();
    throw std::runtime_error("RawInputLoader::LoadEncoded: LibRaw unpack failed");
  }

  const auto kind = ClassifyRawInput(raw->imgdata.rawdata, raw->imgdata.idata);
  const auto pattern =
      kind == RawInputKind::BayerRaw ? ReadLibRawCfaPattern(*raw) : RawCfaPattern{};
  try {
    ThrowIfUnsupported(*raw, kind, pattern);
  } catch (...) {
    raw->recycle();
    throw;
  }

  PreparedRawInput input;
  input.sensor        = SensorFromLibRaw(*raw);
  input.linearization = LinearizationFromLibRaw(*raw);
  FillColorContext(*raw, input.color_context);
  if (dng_metadata.warp_rectilinear.has_value()) {
    input.dng_warp_rectilinear                        = dng_metadata.warp_rectilinear;
    input.color_context.dng_warp_rectilinear_present_ = true;
  }
  if (dng_metadata.default_crop[2] > 0 && dng_metadata.default_crop[3] > 0) {
    std::copy(dng_metadata.default_crop.begin(), dng_metadata.default_crop.end(),
              input.sensor.default_crop);
  }

  if (kind == RawInputKind::DebayeredRgb) {
    input.pixels      = CopyDebayeredRgbPlane(*raw, input.sensor);
    input.host_extent = input.pixels.extent;
    input.input_kind  = RawInputKind::DebayeredRgb;
    CollapseSensorToHost(input.sensor, input.host_extent);
    raw->recycle();
    return FinishPrepared(std::move(input), decode_res, HashContentBytes(encoded), encoded.size());
  }

  const auto& sizes = raw->imgdata.sizes;
  cv::Mat     view(sizes.raw_height, sizes.raw_width, CV_16UC1, raw->imgdata.rawdata.raw_image,
               sizes.raw_pitch != 0
                       ? static_cast<std::size_t>(sizes.raw_pitch)
                       : static_cast<std::size_t>(sizes.raw_width) * sizeof(std::uint16_t));
  input.pixels      = CopyPlane(view, HostPixelFormat::U16Cfa);
  input.host_extent = input.pixels.extent;
  input.input_kind  = RawInputKind::BayerRaw;
  input.cfa_pattern = pattern;
  raw->recycle();
  return FinishPrepared(std::move(input), decode_res, HashContentBytes(encoded), encoded.size());
}

auto RawInputLoader::TryLoadEncoded(std::span<const std::byte> encoded, DecodeRes decode_res)
    -> std::optional<PreparedRawInput> {
  try {
    return LoadEncoded(encoded, decode_res);
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace alcedo
