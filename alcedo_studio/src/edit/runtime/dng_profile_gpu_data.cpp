// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#include "edit/runtime/dng_profile_gpu_data.hpp"

#include <algorithm>
#include <cmath>
#include <opencv2/core.hpp>

namespace alcedo {
auto PackDngProfileGpuData(const DevelopCameraProfile&  profile,
                           const DevelopColorTransform& transform) -> std::vector<float> {
  std::vector<float> result(32, 0.0f);
  if (!profile.dng_profile) return result;
  const auto& dng = *profile.dng_profile;
  result[0]       = 1.0f;
  result[1] = static_cast<float>(std::exp2(dng.baseline_exposure + dng.baseline_exposure_offset));
  const auto append = [&](const DngHueSatMap& map, unsigned descriptor) {
    if (map.entries.empty()) return;
    result[descriptor] = static_cast<float>(result.size());
    for (unsigned i = 0; i < 3; ++i)
      result[descriptor + 1 + i] = static_cast<float>(map.divisions[i]);
    result[descriptor + 4] = static_cast<float>(map.encoding);
    result.insert(result.end(), map.entries.begin(), map.entries.end());
  };
  append(dng.hue_sat_map_1, 2);
  if (!dng.hue_sat_map_2.entries.empty()) {
    const double t1 = profile.color_matrix_1_cct, t2 = profile.color_matrix_2_cct;
    double       weight = 0;
    if (profile.calibration_illuminants_valid && t1 > 0 && t2 > 0 && t1 != t2) {
      weight =
          std::clamp((1.0 / transform.resolved_cct - 1.0 / t1) / (1.0 / t2 - 1.0 / t1), 0.0, 1.0);
    }
    const auto offset = static_cast<std::size_t>(result[2]);
    for (std::size_t i = 0; i < dng.hue_sat_map_1.entries.size(); ++i) {
      result[offset + i] = static_cast<float>((1 - weight) * dng.hue_sat_map_1.entries[i] +
                                              weight * dng.hue_sat_map_2.entries[i]);
    }
  }
  append(dng.look_table, 7);
  const cv::Matx33d prophoto_to_xyz(.7976749, .1351917, .0313534, .2880402, .7118741, .0000857, 0,
                                    0, .8252100);
  cv::Matx33d       xyz_to_ap1;
  for (int i = 0; i < 9; ++i) xyz_to_ap1.val[i] = transform.xyz_d50_to_ap1[i];
  const auto prophoto_to_ap1 = xyz_to_ap1 * prophoto_to_xyz;
  const auto ap1_to_prophoto = prophoto_to_ap1.inv();
  for (int i = 0; i < 9; ++i) {
    result[12 + i] = static_cast<float>(ap1_to_prophoto.val[i]);
    result[21 + i] = static_cast<float>(prophoto_to_ap1.val[i]);
  }
  return result;
}
}  // namespace alcedo
