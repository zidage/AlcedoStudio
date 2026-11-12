// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
// The OpenDRT neutral response is a port of the achromatic path of OpenDRT v1.1.0 by Jed Smith
// (https://github.com/jedypod/open-display-transform), as open_drt_funcs.cuh holds it.
#include "edit/runtime/profile_tone_reference.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <opencv2/core.hpp>
#include <stdexcept>
#include <string>

#include "color/color_encoding_catalog.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/runtime/aces2_reference_math.h"
#include "edit/runtime/dng_profile_gpu_data.hpp"
#include "edit/runtime/drt/drt_output_resolver.hpp"

namespace alcedo {
namespace {

constexpr int    kBisectionSteps = 64;
/// Upper end of the search for the scene value at the peak (2^20). Both default responses reach
/// the peak near scene 128.
constexpr double kSceneSearchMax = 1048576.0;

/// `odrt_spowf`: the power of a positive value; other values pass through.
auto             SafePow(float a, float b) -> float { return a <= 0.0f ? a : std::pow(a, b); }

/// Achromatic path of OpenDRTTransform_fwd for an AP1 neutral of value @p scene.
///
/// On a neutral the normalized RGB is (1, 1, 1): the opponent distance `ach_d` is 0, so the
/// brilliance, hue contrast, hue shift, purity, and post brilliance terms leave it unchanged, the
/// purity mix returns 1, and softplus keeps 1 (its width is below 0.1). The creative white D65 on a
/// Rec.709 display maps it to (1, 1, 1). What remains is the tone scale on the norm `tsn`.
auto             OpenDrtNeutral(const ColorUtils::OpenDRTParams& p, float scene) -> float {
  float tsn = scene + p.tn_off_;
  if (tsn <= 0.0f) return 0.0f;
  tsn = SafePow(tsn / (tsn + p.ts_s_), p.ts_p_);
  tsn *= p.ts_m2_;
  if (p.tn_toe_ != 0.0f) tsn = SafePow(tsn, 2.0f) / (tsn + p.tn_toe_);
  tsn *= p.ts_dsc_;
  return tsn;
}

auto Aces2Neutral(const Aces2ReferenceRuntime& runtime, float scene) -> float {
  if (scene <= 0.0f) return 0.0f;
  const A2rFloat3 display = A2rAp0ToDisplay(A2rMake3(scene, scene, scene), runtime.packed_.data());
  return 0.2126f * display.x + 0.7152f * display.y + 0.0722f * display.z;
}

auto DefaultOpenDrtParams() -> ColorUtils::OpenDRTParams {
  DrtPayload payload;
  payload.method         = DrtMethod::OpenDrt;
  payload.peak_luminance = kProfileToneReferencePeakNits;
  ColorUtils::TO_OUTPUT_Params resolved;
  std::string                  error;
  if (!DrtOutputResolver::Resolve(payload, nullptr, &resolved, &error)) {
    throw std::runtime_error("Profile tone: OpenDRT defaults do not resolve: " + error);
  }
  const auto& p = resolved.open_drt_params_;
  if (p.tn_lcon_enable_ != 0 || p.tn_hcon_enable_ != 0 || p.creative_white_ != 2 ||
      p.display_gamut_ != 0) {
    throw std::runtime_error(
        "Profile tone: the OpenDRT default parameters leave the achromatic port (local or high "
        "contrast enabled, creative white other than D65, or a display other than Rec.709)");
  }
  return p;
}

auto Apply(const std::array<float, 9>& m, const std::array<float, 3>& v) -> std::array<float, 3> {
  return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
          m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

}  // namespace

auto ProfileToneNeutralResponse::Resolve(DrtMethod method)
    -> std::shared_ptr<const ProfileToneNeutralResponse> {
  if (method != DrtMethod::Aces20 && method != DrtMethod::OpenDrt) {
    throw std::invalid_argument("Profile tone: DRT method " +
                                std::to_string(static_cast<int>(method)) +
                                " has no neutral response");
  }
  static std::mutex                                        mutex;
  static std::shared_ptr<const ProfileToneNeutralResponse> responses[2];
  const int                   slot = method == DrtMethod::Aces20 ? 0 : 1;
  std::lock_guard<std::mutex> lock(mutex);
  if (!responses[slot]) {
    std::shared_ptr<ProfileToneNeutralResponse> response(new ProfileToneNeutralResponse());
    response->method_ = method;
    if (method == DrtMethod::Aces20) {
      response->aces_ = ResolveAces2ReferenceRuntime(
          color::GamutPrimariesXy(color::ColorGamutId::Rec709), kProfileToneReferencePeakNits);
      response->scene_at_black_ = 0.0f;
    } else {
      response->open_drt_       = DefaultOpenDrtParams();
      response->scene_at_black_ = -response->open_drt_.tn_off_;
    }
    response->BuildInverseTable();
    responses[slot] = std::move(response);
  }
  return responses[slot];
}

auto ProfileToneNeutralResponse::Forward(float scene) const -> float {
  return method_ == DrtMethod::Aces20 ? Aces2Neutral(*aces_, scene)
                                      : OpenDrtNeutral(open_drt_, scene);
}

void ProfileToneNeutralResponse::BuildInverseTable() {
  // The scene value at the peak: the first power of two at or above it, then bisection.
  double high = 1.0;
  while (Forward(static_cast<float>(high)) < 1.0f) {
    high *= 2.0;
    if (high > kSceneSearchMax) {
      throw std::runtime_error("Profile tone: the neutral response does not reach the peak");
    }
  }
  const auto solve = [&](double display, double low, double up) {
    for (int step = 0; step < kBisectionSteps; ++step) {
      const double mid = 0.5 * (low + up);
      if (Forward(static_cast<float>(mid)) < display) {
        low = mid;
      } else {
        up = mid;
      }
    }
    return 0.5 * (low + up);
  };
  const double black = scene_at_black_;
  const double peak  = solve(1.0, black, high);
  scene_at_peak_     = static_cast<float>(peak);
  inverse_table_.resize(kProfileToneInverseTableEntries);
  inverse_table_.front() = scene_at_black_;
  inverse_table_.back()  = scene_at_peak_;
  const double intervals = static_cast<double>(kProfileToneInverseTableEntries - 1);
  for (std::size_t i = 1; i + 1 < kProfileToneInverseTableEntries; ++i) {
    const double display =
        std::pow(static_cast<double>(i) / intervals, kProfileToneInverseTableExponent);
    inverse_table_[i] = static_cast<float>(solve(display, black, peak));
  }
}

auto ProfileToneNeutralResponse::Inverse(float display) const -> float {
  const float coordinate =
      std::pow(std::clamp(display, 0.0f, 1.0f), 1.0f / kProfileToneInverseTableExponent) *
      static_cast<float>(kProfileToneInverseTableEntries - 1);
  const auto index = static_cast<std::size_t>(coordinate);
  if (index >= kProfileToneInverseTableEntries - 1) return inverse_table_.back();
  const float fract = coordinate - static_cast<float>(index);
  return inverse_table_[index] * (1.0f - fract) + inverse_table_[index + 1] * fract;
}

auto ProfileToneStageMatrices() -> const ProfileToneMatrices& {
  static const ProfileToneMatrices matrices = [] {
    const cv::Matx33d   prophoto_to_ap1 = ProPhotoToAp1(XyzD50ToAp1Matrix());
    const cv::Matx33d   ap1_to_prophoto = prophoto_to_ap1.inv();
    ProfileToneMatrices result;
    for (int i = 0; i < 9; ++i) {
      result.ap1_to_prophoto[i] = static_cast<float>(ap1_to_prophoto.val[i]);
      result.prophoto_to_ap1[i] = static_cast<float>(prophoto_to_ap1.val[i]);
    }
    return result;
  }();
  return matrices;
}

auto ProfileToneHighlightShoulder(float x) -> float {
  // Float, in the order of ProfileToneHighlight: above raw white the curve output lies where the
  // inverse table is steep, so the host and the GPU form keep the same rounding.
  constexpr float k = kProfileToneHighlightKnee;
  if (x <= k) return std::max(x, 0.0f);
  return 1.0f - (1.0f - k) * (1.0f - k) / (x + 1.0f - 2.0f * k);
}

auto ApplyProfileToneStageReference(const DngToneCurve&               curve,
                                    const ProfileToneNeutralResponse& response,
                                    const std::array<float, 3>&       ap1) -> std::array<float, 3> {
  const auto& matrices = ProfileToneStageMatrices();
  const float anchor   = std::exp2(-kProfileToneExposureAnchorEv);
  auto        prophoto = Apply(matrices.ap1_to_prophoto, ap1);
  for (float& channel : prophoto) channel = ProfileToneHighlightShoulder(channel * anchor);
  ApplyDngBaselineRgbTone(curve, prophoto);
  for (float& channel : prophoto) channel = response.Inverse(channel);
  return Apply(matrices.prophoto_to_ap1, prophoto);
}

}  // namespace alcedo
