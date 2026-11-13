// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#include "edit/runtime/profile_tone_stage.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "edit/graph/develop_node_model.hpp"
#include "edit/runtime/profile_tone_gpu_math.h"
#include "edit/runtime/profile_tone_reference.hpp"

namespace alcedo {

static_assert(PROFILE_TONE_TABLE_INTERVALS == kDngToneTableIntervals);
static_assert(PROFILE_TONE_TABLE_INTERVALS + 1 == kProfileToneInverseTableEntries);
static_assert(PROFILE_TONE_INVERSE_TABLE == PROFILE_TONE_CURVE_TABLE + kDngToneTableEntries);
static_assert(PROFILE_TONE_GPU_FLOATS ==
              PROFILE_TONE_INVERSE_TABLE + kProfileToneInverseTableEntries);
static_assert(PROFILE_TONE_HIGHLIGHT_KNEE == kProfileToneHighlightKnee);

auto ResolveProfileToneStage(const PipelineDocument& document) -> std::optional<ProfileToneStage> {
  const auto* drt     = document.Drt();
  const auto* develop = document.Develop();
  if (drt == nullptr || develop == nullptr) return std::nullopt;
  const auto method = drt->Params().Method();
  if (method != DrtMethod::Aces20 && method != DrtMethod::OpenDrt) return std::nullopt;
  const auto reference = develop->Params().CameraProfileFile();
  if (!reference.has_value() || !reference->profile_tone) return std::nullopt;
  if (!reference->IsBound()) {
    throw std::runtime_error("Profile tone: camera profile \"" + reference->profile_name +
                             "\" for \"" + reference->camera_model + "\" is not in the DCP folder");
  }
  // A profile without the tag has no stage (constraint K-5: no default curve).
  if (!reference->profile->profile_tone_curve) return std::nullopt;
  return ProfileToneStage{reference->profile->profile_tone_curve, method};
}

auto PackProfileToneGpuData(const ProfileToneStage& stage) -> std::vector<float> {
  if (!stage.curve) {
    throw std::invalid_argument("Profile tone: the stage has no curve");
  }
  const auto         response = ProfileToneNeutralResponse::Resolve(stage.method);
  const auto&        matrices = ProfileToneStageMatrices();
  std::vector<float> data(PROFILE_TONE_GPU_FLOATS, 0.0f);
  std::copy(matrices.ap1_to_prophoto.begin(), matrices.ap1_to_prophoto.end(),
            data.begin() + PROFILE_TONE_AP1_TO_PROPHOTO);
  std::copy(matrices.prophoto_to_ap1.begin(), matrices.prophoto_to_ap1.end(),
            data.begin() + PROFILE_TONE_PROPHOTO_TO_AP1);
  data[PROFILE_TONE_ANCHOR_SCALE]     = std::exp2(-kProfileToneExposureAnchorEv);
  data[PROFILE_TONE_INVERSE_EXPONENT] = 1.0f / kProfileToneInverseTableExponent;
  std::copy(stage.curve->table.begin(), stage.curve->table.end(),
            data.begin() + PROFILE_TONE_CURVE_TABLE);
  const auto inverse = response->InverseTable();
  std::copy(inverse.begin(), inverse.end(), data.begin() + PROFILE_TONE_INVERSE_TABLE);
  return data;
}

}  // namespace alcedo
