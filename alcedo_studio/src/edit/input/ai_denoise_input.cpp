//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/input/ai_denoise_input.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <opencv2/core/utility.hpp>
#include <stdexcept>
#include <string>

#include "decoders/processor/raw_processor_pattern.hpp"
#include "edit/input/detail/prepared_input_finish.hpp"

namespace alcedo {
namespace {

// XYZ to linear Rec.2020 (the matrix of the lab and of the authors, rawproc.py).
constexpr double kXyzToRec2020[3][3] = {{1.71666343, -0.35567332, -0.25336809},
                                        {-0.66667384, 1.61645574, 0.0157683},
                                        {0.01764248, -0.04277698, 0.94224328}};

/// The area index of extended mosaic index @p e: reflection without the edge pixel.
auto             ReflectIndex(int e, int added, int length) -> int {
  int a = e - added;
  if (a < 0) {
    a = -a;
  } else if (a >= length) {
    a = 2 * (length - 1) - a;
  }
  return a;
}

auto Fail(const std::string& text) -> std::runtime_error {
  return std::runtime_error("AI Denoise input: " + text);
}

/// The value of the shoulder at x = 1 minus 1, for the shoulder width @p shoulder.
auto ShoulderError(double gain, double shoulder) -> double {
  return kAiDenoiseCurveKnee + shoulder * std::log1p((gain - kAiDenoiseCurveKnee) / shoulder) - 1.0;
}

}  // namespace

auto AiDenoiseInputCurve::Apply(double x) const -> double {
  if (IsIdentity()) {
    return x;
  }
  const double knee_x = kAiDenoiseCurveKnee / gain;
  if (x <= knee_x) {
    return gain * x;
  }
  if (x <= 1.0) {
    return kAiDenoiseCurveKnee + shoulder * std::log1p(gain * (x - knee_x) / shoulder);
  }
  return 1.0 + tail_slope * (x - 1.0);
}

auto AiDenoiseInputCurve::Invert(double y) const -> double {
  if (IsIdentity()) {
    return y;
  }
  const double knee_x = kAiDenoiseCurveKnee / gain;
  if (y <= kAiDenoiseCurveKnee) {
    return y / gain;
  }
  if (y <= 1.0) {
    return knee_x + shoulder / gain * std::expm1((y - kAiDenoiseCurveKnee) / shoulder);
  }
  return 1.0 + (y - 1.0) / tail_slope;
}

auto AiDenoiseInputCurveOfGain(double gain) -> AiDenoiseInputCurve {
  if (!std::isfinite(gain)) {
    throw Fail("the input gain " + std::to_string(gain) + " is not finite");
  }
  AiDenoiseInputCurve curve;
  if (gain <= 1.0) {
    return curve;
  }
  // The shoulder value at x = 1 rises with the width: from the knee (width 0) toward the gain
  // (the line). Bisection for the width that gives 1.
  double narrow = 0.0;
  double wide   = 1.0;
  for (int i = 0; i < 200 && ShoulderError(gain, wide) < 0.0; ++i) {
    narrow = wide;
    wide *= 2.0;
  }
  for (int i = 0; i < 200; ++i) {
    const double middle = 0.5 * (narrow + wide);
    if (middle <= narrow || middle >= wide) {
      break;
    }
    (ShoulderError(gain, middle) < 0.0 ? narrow : wide) = middle;
  }
  curve.gain       = gain;
  curve.shoulder   = wide;
  curve.tail_slope = gain / (1.0 + (gain - kAiDenoiseCurveKnee) / wide);
  return curve;
}

auto AiDenoiseInputCurveFor(const std::array<double, 3>& input_means) -> AiDenoiseInputCurve {
  const double level = (input_means[0] + input_means[1] + input_means[2]) / 3.0;
  if (!std::isfinite(level)) {
    throw Fail("the input level " + std::to_string(level) + " is not finite");
  }
  const double gain = level * kAiDenoiseMaxInputGain <= kAiDenoiseTargetInputLevel
                          ? kAiDenoiseMaxInputGain
                          : kAiDenoiseTargetInputLevel / level;
  return AiDenoiseInputCurveOfGain(std::clamp(gain, 1.0, kAiDenoiseMaxInputGain));
}

auto PackBayerForAiDenoise(const PreparedRawInput& cfa) -> AiDenoisePackedMosaic {
  if (cfa.input_kind != RawInputKind::BayerRaw || cfa.cfa_pattern.kind != RawCfaKind::Bayer2x2 ||
      !IsClassic2x2Bayer(cfa.cfa_pattern.bayer_pattern)) {
    throw Fail("a Bayer CFA input is required");
  }
  if (cfa.downsample_passes != 0 || cfa.pixels.format != HostPixelFormat::U16Cfa ||
      !cfa.pixels.bytes) {
    throw Fail("a full-size U16 CFA input is required");
  }
  const auto& linearization = cfa.linearization;
  for (int c = 0; c < 4; ++c) {
    if (!(linearization.libraw_maximum > linearization.black_level[c])) {
      throw Fail("the LibRaw maximum " + std::to_string(linearization.libraw_maximum) +
                 " is not above the black level " + std::to_string(linearization.black_level[c]) +
                 " of color " + std::to_string(c));
    }
  }
  const RectI area = input_detail::FullSizeDecodeArea(cfa.sensor, cfa.host_extent);
  if (area.width < 2 || area.height < 2) {
    throw Fail("the decode area is smaller than 2 x 2 px");
  }

  const auto& pattern = cfa.cfa_pattern.bayer_pattern;
  // The position of R in the 2 x 2 cell at the origin of the area.
  int         red_y   = -1;
  int         red_x   = -1;
  for (int dy = 0; dy < 2; ++dy) {
    for (int dx = 0; dx < 2; ++dx) {
      if (RgbColorAt(pattern, area.y + dy, area.x + dx) == 0) {
        red_y = dy;
        red_x = dx;
      }
    }
  }
  if (red_y < 0) {
    throw Fail("the CFA pattern has no red position");
  }

  AiDenoisePackedMosaic mosaic;
  mosaic.added_top          = red_y;
  mosaic.added_left         = red_x;
  mosaic.area_width         = area.width;
  mosaic.area_height        = area.height;
  const int extended_height = area.height + red_y + (area.height + red_y) % 2;
  const int extended_width  = area.width + red_x + (area.width + red_x) % 2;
  mosaic.height             = extended_height / 2;
  mosaic.width              = extended_width / 2;
  const auto plane          = static_cast<std::size_t>(mosaic.width) * mosaic.height;
  mosaic.planes.resize(4 * plane);

  const auto* samples = reinterpret_cast<const std::uint16_t*>(cfa.pixels.bytes.get());
  const auto  stride  = cfa.pixels.stride_bytes / sizeof(std::uint16_t);
  const bool  pattern_black_on =
      linearization.black_tile_width > 0 && linearization.black_tile_height > 0;
  // The prepared value of host position (y, x).
  const auto prepared = [&](int y, int x) -> float {
    const int   raw_color = RawColorAt(pattern, y, x);
    float       black     = linearization.black_level[raw_color];
    const float range     = linearization.libraw_maximum - black;
    if (pattern_black_on) {
      black += linearization.pattern_black[(y % linearization.black_tile_height) *
                                               linearization.black_tile_width +
                                           x % linearization.black_tile_width];
    }
    const float sample = static_cast<float>(samples[static_cast<std::size_t>(y) * stride + x]);
    return (sample - black) / range;
  };

  // Each packed row reads two extended mosaic rows; the means come from the rows of the area.
  struct RowSums {
    std::array<double, 3> sums{};
    std::array<double, 3> counts{};
  };
  std::vector<RowSums> row_sums(static_cast<std::size_t>(mosaic.height));
  cv::parallel_for_(cv::Range(0, mosaic.height), [&](const cv::Range& rows) {
    for (int py = rows.start; py < rows.end; ++py) {
      RowSums& row = row_sums[static_cast<std::size_t>(py)];
      for (int dy = 0; dy < 2; ++dy) {
        const int  ey       = 2 * py + dy;
        const int  ay       = ReflectIndex(ey, mosaic.added_top, area.height);
        const bool area_row = ey - mosaic.added_top == ay;
        for (int px = 0; px < mosaic.width; ++px) {
          for (int dx = 0; dx < 2; ++dx) {
            const int   ex      = 2 * px + dx;
            const int   ax      = ReflectIndex(ex, mosaic.added_left, area.width);
            const float value   = prepared(area.y + ay, area.x + ax);
            const int   channel = 2 * dy + dx;
            mosaic.planes[channel * plane + static_cast<std::size_t>(py) * mosaic.width + px] =
                value;
            if (area_row && ex - mosaic.added_left == ax) {
              const int color = channel == 0 ? 0 : (channel == 3 ? 2 : 1);
              row.sums[color] += value;
              row.counts[color] += 1.0;
            }
          }
        }
      }
    }
  });
  std::array<double, 3> sums{};
  std::array<double, 3> counts{};
  for (const auto& row : row_sums) {
    for (int c = 0; c < 3; ++c) {
      sums[c] += row.sums[c];
      counts[c] += row.counts[c];
    }
  }
  for (int c = 0; c < 3; ++c) {
    mosaic.input_means[c] = sums[c] / counts[c];
  }
  return mosaic;
}

void ApplyAiDenoiseInputCurve(AiDenoisePackedMosaic& mosaic, const AiDenoiseInputCurve& curve) {
  if (!mosaic.curve.IsIdentity()) {
    throw Fail("the input already has an exposure curve");
  }
  mosaic.curve = curve;
  if (curve.IsIdentity()) {
    return;
  }
  const auto plane = static_cast<std::size_t>(mosaic.width) * mosaic.height;
  // The sums of each packed row, added in row order (the same means for each thread count).
  struct RowSums {
    std::array<double, 3> sums{};
    std::array<double, 3> counts{};
  };
  std::vector<RowSums> row_sums(static_cast<std::size_t>(mosaic.height));
  cv::parallel_for_(cv::Range(0, mosaic.height), [&](const cv::Range& rows) {
    for (int py = rows.start; py < rows.end; ++py) {
      RowSums& row = row_sums[static_cast<std::size_t>(py)];
      for (int channel = 0; channel < 4; ++channel) {
        const int  ay       = 2 * py + channel / 2 - mosaic.added_top;
        const bool area_row = ay >= 0 && ay < mosaic.area_height;
        const int  color    = channel == 0 ? 0 : (channel == 3 ? 2 : 1);
        float*     values =
            mosaic.planes.data() + channel * plane + static_cast<std::size_t>(py) * mosaic.width;
        for (int px = 0; px < mosaic.width; ++px) {
          const auto value = static_cast<float>(curve.Apply(values[px]));
          values[px]       = value;
          const int ax     = 2 * px + channel % 2 - mosaic.added_left;
          if (area_row && ax >= 0 && ax < mosaic.area_width) {
            row.sums[color] += value;
            row.counts[color] += 1.0;
          }
        }
      }
    }
  });
  std::array<double, 3> sums{};
  std::array<double, 3> counts{};
  for (const auto& row : row_sums) {
    for (int c = 0; c < 3; ++c) {
      sums[c] += row.sums[c];
      counts[c] += row.counts[c];
    }
  }
  for (int c = 0; c < 3; ++c) {
    mosaic.input_means[c] = sums[c] / counts[c];
  }
}

auto AiDenoiseGainWeights(const std::array<double, 9>& xyz_to_camera) -> std::array<double, 3> {
  cv::Matx33d camera_from_xyz;
  for (int i = 0; i < 9; ++i) {
    camera_from_xyz.val[i] = xyz_to_camera[static_cast<std::size_t>(i)];
  }
  const double determinant = cv::determinant(camera_from_xyz);
  if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12) {
    throw Fail("the camera matrix is singular");
  }
  const cv::Matx33d xyz_from_camera = camera_from_xyz.inv();
  cv::Matx33d       xyz_to_rec2020;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      xyz_to_rec2020(r, c) = kXyzToRec2020[r][c];
    }
  }
  const cv::Matx33d     rec2020_from_camera = xyz_to_rec2020 * xyz_from_camera;
  std::array<double, 3> weights{};
  for (int c = 0; c < 3; ++c) {
    weights[c] = rec2020_from_camera(0, c) + rec2020_from_camera(1, c) + rec2020_from_camera(2, c);
    if (!std::isfinite(weights[c])) {
      throw Fail("the camera matrix is not finite");
    }
  }
  return weights;
}

AiDenoiseOutputSums::AiDenoiseOutputSums(const AiDenoisePackedMosaic& mosaic)
    : area_{mosaic.added_left, mosaic.added_top, mosaic.area_width, mosaic.area_height} {}

void AiDenoiseOutputSums::AddKept(const nn::RgbPlanesView& output, const nn::PackedTileRect& kept) {
  const int left   = std::max(2 * kept.x, area_.x);
  const int top    = std::max(2 * kept.y, area_.y);
  const int right  = std::min(2 * (kept.x + kept.width), area_.x + area_.width);
  const int bottom = std::min(2 * (kept.y + kept.height), area_.y + area_.height);
  if (right <= left || bottom <= top) {
    return;
  }
  const auto plane = static_cast<std::size_t>(output.width) * output.height;
  std::mutex mutex;
  cv::parallel_for_(cv::Range(top, bottom), [&](const cv::Range& rows) {
    std::array<double, 3> local{};
    for (int y = rows.start; y < rows.end; ++y) {
      for (int c = 0; c < 3; ++c) {
        const float* row =
            output.data.data() + c * plane + static_cast<std::size_t>(y) * output.width;
        double sum = 0.0;
        for (int x = left; x < right; ++x) {
          sum += row[x];
        }
        local[c] += sum;
      }
    }
    std::scoped_lock lock(mutex);
    for (int c = 0; c < 3; ++c) {
      sums_[c] += local[c];
    }
  });
  pixels_ += static_cast<double>(right - left) * (bottom - top);
}

auto AiDenoiseOutputSums::Means() const -> std::array<double, 3> {
  if (pixels_ <= 0.0) {
    throw Fail("no output px of the area was added");
  }
  return {sums_[0] / pixels_, sums_[1] / pixels_, sums_[2] / pixels_};
}

auto AiDenoiseGainFactor(const std::array<double, 3>& weights,
                         const std::array<double, 3>& input_means,
                         const std::array<double, 3>& output_means) -> double {
  double input  = 0.0;
  double output = 0.0;
  for (int c = 0; c < 3; ++c) {
    input += weights[c] * input_means[c];
    output += weights[c] * output_means[c];
  }
  const double factor = input / output;
  if (!std::isfinite(factor) || factor <= 0.0) {
    throw Fail("the gain match factor " + std::to_string(factor) +
               " is not a finite value above 0");
  }
  return factor;
}

auto AssembleDenoisedPlane(const nn::RgbPlanesView& output, const AiDenoisePackedMosaic& mosaic,
                           double gain, const RawLinearizationParams& linearization)
    -> HostImagePlane {
  if (output.width != mosaic.OutputWidth() || output.height != mosaic.OutputHeight() ||
      output.data.size() != 3 * static_cast<std::size_t>(output.width) * output.height) {
    throw Fail("the model output does not match the packed mosaic");
  }
  std::array<double, 3> scale{};
  for (int c = 0; c < 3; ++c) {
    const double white_range =
        static_cast<double>(linearization.white_level[c]) - linearization.black_level[c];
    if (!(white_range > 0.0)) {
      throw Fail("the white level of color " + std::to_string(c) + " is not above its black level");
    }
    scale[c] = (static_cast<double>(linearization.libraw_maximum) - linearization.black_level[c]) /
               white_range;
  }
  const AiDenoiseInputCurve& curve = mosaic.curve;
  // The gain-matched output is in the domain of the curve; the inverse curve returns it to the
  // prepared values.
  const auto                 value = [&](float output, int c) {
    return static_cast<float>(curve.Invert(gain * output) * scale[c]);
  };

  HostImagePlane plane;
  plane.extent            = Extent2D{static_cast<std::uint32_t>(mosaic.area_width),
                          static_cast<std::uint32_t>(mosaic.area_height)};
  plane.stride_bytes      = plane.extent.width * 4U * static_cast<std::uint32_t>(sizeof(float));
  plane.format            = HostPixelFormat::F32Rgba;
  auto       storage      = std::shared_ptr<std::byte>(new std::byte[plane.ByteCount()],
                                                       std::default_delete<std::byte[]>());
  auto*      target       = reinterpret_cast<float*>(storage.get());
  const auto source_plane = static_cast<std::size_t>(output.width) * output.height;
  cv::parallel_for_(cv::Range(0, mosaic.area_height), [&](const cv::Range& rows) {
    for (int y = rows.start; y < rows.end; ++y) {
      float*     row    = target + static_cast<std::size_t>(y) * mosaic.area_width * 4;
      const auto offset = static_cast<std::size_t>(y + mosaic.added_top) * output.width +
                          static_cast<std::size_t>(mosaic.added_left);
      const float* r = output.data.data() + offset;
      const float* g = r + source_plane;
      const float* b = g + source_plane;
      for (int x = 0; x < mosaic.area_width; ++x) {
        row[4 * x + 0] = value(r[x], 0);
        row[4 * x + 1] = value(g[x], 1);
        row[4 * x + 2] = value(b[x], 2);
        row[4 * x + 3] = 1.0f;
      }
    }
  });
  plane.bytes = std::const_pointer_cast<const std::byte>(storage);
  return plane;
}

}  // namespace alcedo
