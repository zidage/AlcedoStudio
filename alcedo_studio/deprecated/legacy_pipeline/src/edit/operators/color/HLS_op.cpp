//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/color/HLS_op.hpp"

#include <opencv2/core/hal/interface.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "edit/operators/operator_factory.hpp"
#include "image/image_buffer.hpp"
#include "json.hpp"

namespace alcedo {
namespace {
constexpr int kHlsProfileCount = OperatorParams::kHlsProfileCount;
constexpr float kDefaultHueSmoothness = 45.0f;
constexpr std::array<float, kHlsProfileCount> kDefaultHueProfiles = {
    0.0f, 45.0f, 90.0f, 135.0f, 180.0f, 225.0f, 270.0f, 315.0f};

auto WrapHueDegrees(float hue) -> float {
  hue = std::fmod(hue, 360.0f);
  if (hue < 0.0f) {
    hue += 360.0f;
  }
  return hue;
}

auto HueDistanceDegrees(float a, float b) -> float {
  const float diff = std::abs(WrapHueDegrees(a) - WrapHueDegrees(b));
  return std::min(diff, 360.0f - diff);
}

auto Smoothstep(float edge0, float edge1, float x) -> float {
  const float denom = std::max(edge1 - edge0, 1e-6f);
  const float t     = std::clamp((x - edge0) / denom, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

auto SoftFloor(float x, float floor, float softness) -> float {
  const float t = (x - floor) / std::max(softness, 1e-6f);
  if (t > 20.0f) {
    return x;
  }
  if (t < -20.0f) {
    return floor;
  }
  return floor + softness * std::log1p(std::exp(t));
}

auto AcesccDecode(float acescc) -> float {
  constexpr float kLog2Min      = -15.0f;
  constexpr float kLog2Denorm   = -16.0f;
  constexpr float kDenormOffset = 0.00001525878906f;
  constexpr float kA            = 9.72f;
  constexpr float kB            = 17.52f;

  const float encode_floor     = (kLog2Denorm + kA) / kB;
  const float denorm_threshold = (kLog2Min + kA) / kB;
  if (acescc < encode_floor) {
    return acescc - encode_floor;
  }
  if (acescc <= denorm_threshold) {
    return (std::exp2(acescc * kB - kA) - kDenormOffset) * 2.0f;
  }
  return std::exp2(acescc * kB - kA);
}

auto AcesccEncode(float linear_ap1) -> float {
  constexpr float kLog2Denorm   = -16.0f;
  constexpr float kDenormTrans  = 0.00003051757812f;
  constexpr float kDenormOffset = 0.00001525878906f;
  constexpr float kA            = 9.72f;
  constexpr float kB            = 17.52f;

  const float encode_floor = (kLog2Denorm + kA) / kB;
  if (linear_ap1 <= 0.0f) {
    return encode_floor + linear_ap1;
  }
  if (linear_ap1 < kDenormTrans) {
    return (std::log2(kDenormOffset + linear_ap1 * 0.5f) + kA) / kB;
  }
  return (std::log2(linear_ap1) + kA) / kB;
}

auto AcesccToAp1(const cv::Vec3f& acescc) -> cv::Vec3f {
  return {AcesccDecode(acescc[0]), AcesccDecode(acescc[1]), AcesccDecode(acescc[2])};
}

auto Ap1ToAcescc(const cv::Vec3f& ap1) -> cv::Vec3f {
  return {AcesccEncode(ap1[0]), AcesccEncode(ap1[1]), AcesccEncode(ap1[2])};
}

auto Ap1ToOklab(const cv::Vec3f& ap1) -> cv::Vec3f {
  const float l = 0.62217537f * ap1[0] + 0.34268438f * ap1[1] + 0.02339492f * ap1[2];
  const float m = 0.26593478f * ap1[0] + 0.62930460f * ap1[1] + 0.10828100f * ap1[2];
  const float s = 0.09725037f * ap1[0] + 0.18525749f * ap1[1] + 0.77254586f * ap1[2];

  const float l_ = std::cbrt(l);
  const float m_ = std::cbrt(m);
  const float s_ = std::cbrt(s);

  return {0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
          1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
          0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_};
}

auto OklabToAp1(const cv::Vec3f& lab) -> cv::Vec3f {
  const float l_ = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
  const float m_ = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
  const float s_ = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];

  const float l = l_ * l_ * l_;
  const float m = m_ * m_ * m_;
  const float s = s_ * s_ * s_;

  return {2.09085732f * l - 1.16812363f * m + 0.10040848f * s,
          -0.87435428f * l + 2.14592958f * m - 0.27429822f * s,
          -0.05353206f * l - 0.36754978f * m + 1.34755888f * s};
}

auto FitAp1LowerGamut(const cv::Vec3f& adjusted_ap1, const cv::Vec3f& neutral_ap1) -> cv::Vec3f {
  constexpr float kLower = -1e-5f;
  float           scale  = 1.0f;

  for (int i = 0; i < 3; ++i) {
    if (adjusted_ap1[i] < kLower && neutral_ap1[i] > adjusted_ap1[i]) {
      scale = std::min(scale, (neutral_ap1[i] - kLower) / (neutral_ap1[i] - adjusted_ap1[i]));
    }
  }

  scale = std::clamp(scale, 0.0f, 1.0f);
  return {neutral_ap1[0] + (adjusted_ap1[0] - neutral_ap1[0]) * scale,
          neutral_ap1[1] + (adjusted_ap1[1] - neutral_ap1[1]) * scale,
          neutral_ap1[2] + (adjusted_ap1[2] - neutral_ap1[2]) * scale};
}

auto EvaluateHueCurve(float hue, const std::array<float, kHlsProfileCount>& profile_hues,
                      const std::array<cv::Vec3f, kHlsProfileCount>& adjustments,
                      const std::array<float, kHlsProfileCount>& widths) -> cv::Vec3f {
  constexpr float kEps = 1e-6f;

  float sum_h      = 0.0f;
  float sum_l      = 0.0f;
  float sum_c      = 0.0f;
  float sum_weight = 0.0f;
  int   nearest    = 0;
  float nearest_d  = HueDistanceDegrees(hue, profile_hues[0]);

  for (int i = 0; i < kHlsProfileCount; ++i) {
    const float dist = HueDistanceDegrees(hue, profile_hues[i]);
    if (dist < nearest_d) {
      nearest_d = dist;
      nearest   = i;
    }

    const float width  = std::max(widths[i], 1.0f);
    const float t      = dist / width;
    const float weight = std::exp2(-(t * t));
    sum_h += adjustments[i][0] * weight;
    sum_l += adjustments[i][1] * weight;
    sum_c += adjustments[i][2] * weight;
    sum_weight += weight;
  }

  if (sum_weight <= kEps) {
    return adjustments[nearest];
  }

  const float inv_weight = 1.0f / sum_weight;
  return {sum_h * inv_weight, sum_l * inv_weight, sum_c * inv_weight};
}

auto ApplyHlsOklchCurve(const cv::Vec3f& source_acescc,
                        const std::array<float, kHlsProfileCount>& profile_hues,
                        const std::array<cv::Vec3f, kHlsProfileCount>& adjustments,
                        const std::array<float, kHlsProfileCount>& widths) -> cv::Vec3f {
  constexpr float kEps                = 1e-6f;
  constexpr float kPi                 = 3.14159265358979323846f;
  constexpr float kCurveGain          = 2.25f;
  constexpr float kLightnessScale     = 0.5f;
  constexpr float kChromaScalePos     = 4.5f;
  constexpr float kChromaScaleNeg     = 3.25f;

  const cv::Vec3f source_ap1    = AcesccToAp1(source_acescc);
  const cv::Vec3f source_lab    = Ap1ToOklab(source_ap1);
  const float     source_chroma = std::hypot(source_lab[1], source_lab[2]);
  if (source_chroma <= kEps) {
    return source_acescc;
  }

  const float source_hue = WrapHueDegrees(std::atan2(source_lab[2], source_lab[1]) * 180.0f / kPi);
  const cv::Vec3f curve  = EvaluateHueCurve(source_hue, profile_hues, adjustments, widths);

  const float chroma_confidence    = Smoothstep(0.005f, 0.030f, source_chroma);
  const float shadow_confidence    = Smoothstep(0.005f, 0.050f, source_lab[0]);
  const float highlight_confidence = 1.0f - Smoothstep(1.35f, 2.25f, source_lab[0]);
  const float protection           =
      std::clamp(chroma_confidence * shadow_confidence * highlight_confidence, 0.0f, 1.0f);
  if (protection <= kEps) {
    return source_acescc;
  }

  const float adjusted_hue_rad =
      WrapHueDegrees(source_hue + curve[0] * kCurveGain * protection) * (kPi / 180.0f);
  const float adjusted_lightness =
      SoftFloor(source_lab[0] + curve[1] * kCurveGain * kLightnessScale * protection, 0.0f,
                0.02f);
  const float chroma_scale    = (curve[2] >= 0.0f) ? kChromaScalePos : kChromaScaleNeg;
  const float adjusted_chroma =
      source_chroma * std::exp2(curve[2] * kCurveGain * chroma_scale * protection);

  const cv::Vec3f adjusted_lab = {adjusted_lightness, adjusted_chroma * std::cos(adjusted_hue_rad),
                                  adjusted_chroma * std::sin(adjusted_hue_rad)};
  const cv::Vec3f neutral_lab  = {adjusted_lightness, 0.0f, 0.0f};
  const cv::Vec3f output_ap1 =
      FitAp1LowerGamut(OklabToAp1(adjusted_lab), OklabToAp1(neutral_lab));
  return Ap1ToAcescc(output_ap1);
}

auto ClosestHueProfileIdx(float hue, const std::array<float, kHlsProfileCount>& profiles) -> int {
  int   best_idx  = 0;
  float best_dist = HueDistanceDegrees(hue, profiles[0]);
  for (int i = 1; i < kHlsProfileCount; ++i) {
    const float dist = HueDistanceDegrees(hue, profiles[i]);
    if (dist < best_dist) {
      best_dist = dist;
      best_idx  = i;
    }
  }
  return best_idx;
}
}  // namespace

HLSOp::HLSOp()
    : target_hls_(0, 0.5f, 1.0f),
      hls_adjustment_(0.0f, 0.0f, 0.0f),
      hue_range_(kDefaultHueSmoothness),
      lightness_range_(0.1f),
      saturation_range_(0.1f) {
  hue_profile_values_ = kDefaultHueProfiles;
  hls_adjustment_table_.fill(cv::Vec3f(0.0f, 0.0f, 0.0f));
  hue_range_table_.fill(kDefaultHueSmoothness);
  active_profile_idx_ = 0;
}

HLSOp::HLSOp(const nlohmann::json& params) { SetParams(params); }

void HLSOp::SetTargetColor(const cv::Vec3f& bgr_color_normalized) {
  cv::Mat bgr_mat(1, 1, CV_32FC3);
  bgr_mat.at<cv::Vec3f>(0, 0) = bgr_color_normalized;

  cv::Mat HLS_mat;
  cv::cvtColor(bgr_mat, HLS_mat, cv::COLOR_BGR2HLS);
  target_hls_ = HLS_mat.at<cv::Vec3f>(0, 0);
  active_profile_idx_ = ClosestHueProfileIdx(target_hls_[0], hue_profile_values_);
  target_hls_[0]      = hue_profile_values_[active_profile_idx_];
  hls_adjustment_     = hls_adjustment_table_[active_profile_idx_];
  hue_range_          = hue_range_table_[active_profile_idx_];
}

void HLSOp::SetAdjustment(const cv::Vec3f& adjustment) {
  hls_adjustment_ = adjustment;
  if (active_profile_idx_ >= 0 && active_profile_idx_ < kHlsProfileCount) {
    hls_adjustment_table_[active_profile_idx_] = adjustment;
  }
}

void HLSOp::SetRanges(float h_range, float l_range, float s_range) {
  hue_range_        = h_range;
  lightness_range_  = l_range;
  saturation_range_ = s_range;
  if (active_profile_idx_ >= 0 && active_profile_idx_ < kHlsProfileCount) {
    hue_range_table_[active_profile_idx_] = h_range;
  }
}

void HLSOp::Apply(std::shared_ptr<ImageBuffer> input) {
  bool has_any_adjustment = false;
  for (const auto& adj : hls_adjustment_table_) {
    if (cv::norm(adj, cv::NORM_L2SQR) >= 1e-10) {
      has_any_adjustment = true;
      break;
    }
  }
  if (!has_any_adjustment) {
    return;
  }

  cv::Mat& img = input->GetCPUData();
  if (img.empty() || img.depth() != CV_32F || img.channels() < 3) {
    return;
  }

  const int channels = img.channels();
  for (int y = 0; y < img.rows; ++y) {
    float* row = img.ptr<float>(y);
    for (int x = 0; x < img.cols; ++x) {
      float* pixel = row + x * channels;
      const cv::Vec3f adjusted =
          ApplyHlsOklchCurve({pixel[0], pixel[1], pixel[2]}, hue_profile_values_,
                             hls_adjustment_table_, hue_range_table_);
      pixel[0] = adjusted[0];
      pixel[1] = adjusted[1];
      pixel[2] = adjusted[2];
    }
  }
}

void HLSOp::ApplyGPU(std::shared_ptr<ImageBuffer>) {
  throw std::runtime_error("HLSOp: ApplyGPU not implemented");
}

auto HLSOp::GetParams() const -> nlohmann::json {
  nlohmann::json o;
  nlohmann::json inner;

  nlohmann::json hue_bins     = nlohmann::json::array();
  nlohmann::json adj_table    = nlohmann::json::array();
  nlohmann::json h_range_table = nlohmann::json::array();
  for (int i = 0; i < kHlsProfileCount; ++i) {
    hue_bins.push_back(hue_profile_values_[i]);
    adj_table.push_back(
        std::array<float, 3>{hls_adjustment_table_[i][0], hls_adjustment_table_[i][1],
                             hls_adjustment_table_[i][2]});
    h_range_table.push_back(hue_range_table_[i]);
  }
  inner["hue_bins"]       = std::move(hue_bins);
  inner["hls_adj_table"]  = std::move(adj_table);
  inner["h_range_table"]  = std::move(h_range_table);

  const int active_idx = std::clamp(active_profile_idx_, 0, kHlsProfileCount - 1);
  inner["target_hls"]  = std::array<float, 3>{hue_profile_values_[active_idx], target_hls_[1],
                                               target_hls_[2]};
  inner["hls_adj"] =
      std::array<float, 3>{hls_adjustment_table_[active_idx][0], hls_adjustment_table_[active_idx][1],
                           hls_adjustment_table_[active_idx][2]};
  inner["h_range"] = hue_range_table_[active_idx];
  inner["l_range"] = lightness_range_;
  inner["s_range"] = saturation_range_;

  o[script_name_]  = inner;
  return o;
}

void HLSOp::SetParams(const nlohmann::json& params) {
  target_hls_        = {0.0f, 0.5f, 1.0f};
  hls_adjustment_    = {0.0f, 0.0f, 0.0f};
  hue_range_         = kDefaultHueSmoothness;
  lightness_range_   = 0.1f;
  saturation_range_  = 0.1f;
  hue_profile_values_ = kDefaultHueProfiles;
  hls_adjustment_table_.fill(cv::Vec3f(0.0f, 0.0f, 0.0f));
  hue_range_table_.fill(kDefaultHueSmoothness);
  active_profile_idx_ = 0;

  if (!params.contains(script_name_)) {
    return;
  }

  nlohmann::json inner          = params[script_name_];
  bool           has_adj_table  = false;
  bool           has_range_table = false;

  if (inner.contains("hue_bins") && inner["hue_bins"].is_array()) {
    const auto& bins  = inner["hue_bins"];
    const int   count = std::min<int>(kHlsProfileCount, static_cast<int>(bins.size()));
    for (int i = 0; i < count; ++i) {
      try {
        hue_profile_values_[i] = WrapHueDegrees(bins[i].get<float>());
      } catch (...) {
      }
    }
  }

  if (inner.contains("hls_adj_table") && inner["hls_adj_table"].is_array()) {
    const auto& tbl  = inner["hls_adj_table"];
    const int   count = std::min<int>(kHlsProfileCount, static_cast<int>(tbl.size()));
    for (int i = 0; i < count; ++i) {
      try {
        if (tbl[i].is_array() && tbl[i].size() >= 3) {
          hls_adjustment_table_[i] =
              cv::Vec3f(tbl[i][0].get<float>(), tbl[i][1].get<float>(), tbl[i][2].get<float>());
          has_adj_table = true;
        }
      } catch (...) {
      }
    }
  }

  if (inner.contains("h_range_table") && inner["h_range_table"].is_array()) {
    const auto& tbl  = inner["h_range_table"];
    const int   count = std::min<int>(kHlsProfileCount, static_cast<int>(tbl.size()));
    for (int i = 0; i < count; ++i) {
      try {
        hue_range_table_[i] = std::max(tbl[i].get<float>(), 1e-6f);
        has_range_table     = true;
      } catch (...) {
      }
    }
  }

  if (inner.contains("target_hls")) {
    try {
      auto tgt_hls      = inner["target_hls"].get<std::array<float, 3>>();
      target_hls_       = {tgt_hls[0], tgt_hls[1], tgt_hls[2]};
      active_profile_idx_ = ClosestHueProfileIdx(target_hls_[0], hue_profile_values_);
      target_hls_[0]    = hue_profile_values_[active_profile_idx_];
    } catch (...) {
    }
  }
  if (inner.contains("hls_adj")) {
    try {
      auto hls_adj      = inner["hls_adj"].get<std::array<float, 3>>();
      hls_adjustment_   = {hls_adj[0], hls_adj[1], hls_adj[2]};
      if (!has_adj_table) {
        hls_adjustment_table_[active_profile_idx_] = hls_adjustment_;
      }
    } catch (...) {
    }
  }
  if (inner.contains("h_range")) {
    try {
      hue_range_        = inner["h_range"].get<float>();
      if (!has_range_table) {
        hue_range_table_[active_profile_idx_] = std::max(hue_range_, 1e-6f);
      }
    } catch (...) {
    }
  }
  if (inner.contains("l_range")) {
    try {
      lightness_range_ = inner["l_range"].get<float>();
    } catch (...) {
    }
  }
  if (inner.contains("s_range")) {
    try {
      saturation_range_ = inner["s_range"].get<float>();
    } catch (...) {
    }
  }

  active_profile_idx_ = std::clamp(active_profile_idx_, 0, kHlsProfileCount - 1);
  hls_adjustment_     = hls_adjustment_table_[active_profile_idx_];
  hue_range_          = hue_range_table_[active_profile_idx_];
  target_hls_[0]      = hue_profile_values_[active_profile_idx_];
}

void HLSOp::SetGlobalParams(OperatorParams& params) const {
  const int active_idx = std::clamp(active_profile_idx_, 0, kHlsProfileCount - 1);
  params.target_hls_[0]     = hue_profile_values_[active_idx];
  params.target_hls_[1]     = target_hls_[1];
  params.target_hls_[2]     = target_hls_[2];

  params.hls_adjustment_[0] = hls_adjustment_table_[active_idx][0];
  params.hls_adjustment_[1] = hls_adjustment_table_[active_idx][1];
  params.hls_adjustment_[2] = hls_adjustment_table_[active_idx][2];

  params.hue_range_         = hue_range_table_[active_idx];
  params.lightness_range_   = lightness_range_;
  params.saturation_range_  = saturation_range_;
  params.hls_profile_count_ = kHlsProfileCount;
  for (int i = 0; i < kHlsProfileCount; ++i) {
    params.hls_profile_hues_[i]            = hue_profile_values_[i];
    params.hls_profile_adjustments_[i][0]  = hls_adjustment_table_[i][0];
    params.hls_profile_adjustments_[i][1]  = hls_adjustment_table_[i][1];
    params.hls_profile_adjustments_[i][2]  = hls_adjustment_table_[i][2];
    params.hls_profile_hue_ranges_[i]      = hue_range_table_[i];
  }
}

void HLSOp::EnableGlobalParams(OperatorParams& params, bool enable) {
  params.hls_enabled_ = enable;
}
};  // namespace alcedo
